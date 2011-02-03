/**
   @file validatorlistener.c

   Listen to Validator messages from kernel during startup.
   Validator messages are received via netlink socket.
   Stop listening when init signals that we are about to
   launch 3rd party daemons.
   <p>
   Copyright (C) 2011 Nokia Corporation.

   @author Semi Malinen <semi.malinen@nokia.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

// to stop listening to Validator:
// dbus-send --system --type=signal /com/nokia/startup/signal com.nokia.startup.signal.base_boot_done

#include <dsme/protocol.h>

#include "dsme_dbus.h"
#include "dbusproxy.h"

#include "malf.h"
#include "dsme/modules.h"
#include "dsme/logging.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <glib.h>
#include <ctype.h>
#include <errno.h>


// TODO: try to find a header that #defines NETLINK_VALIDATOR
//       and possibly the right group mask to use
#ifndef NETLINK_VALIDATOR
#define NETLINK_VALIDATOR 25
#endif
#ifndef VALIDATOR_MAX_PAYLOAD
#define VALIDATOR_MAX_PAYLOAD 4096
#endif


static void stop_listening_to_validator(void);


static int         validator_fd = -1;
static GIOChannel* channel      = 0;


static void go_to_malf(const char* component, const char* details)
{
    DSM_MSGTYPE_ENTER_MALF malf = DSME_MSG_INIT(DSM_MSGTYPE_ENTER_MALF);
    malf.reason          = DSME_MALF_SECURITY;
    malf.component       = component;

    broadcast_internally_with_extra(&malf, strlen(details) + 1, details);
}


// parse a line of format "<key>: <text>"
static bool parse_validator_line(const char** msg, char** key, char** text)
{
    bool parsed = false;

    const char* p;
    if ((p = strchr(*msg, ':'))) {
        // got the key
        *key = strndup(*msg, p - *msg);

        // skip whitespace
        do {
            ++p;
        } while (*p && *p != '\n' && isblank((unsigned char)*p));

        // got the beginning of text; now determine where it ends
        const char* t = p;
        while (*p && *p != '\n') {
            ++p;
        }
        *text = strndup(t, p - t);

        // move to the next line if necessary, and save the parsing point
        if (*p == '\n') {
            ++p;
        }
        *msg = p;

        parsed = true;
    }

    return parsed;
}

static void parse_validator_message(const char* msg,
                                    char**      component,
                                    char**      details)
{
    *component = 0;
    *details   = 0;

    const char* p = msg;
    char*       key;
    char*       text;
    while (p && *p && parse_validator_line(&p, &key, &text)) {
        if (strcmp(key, "Process") == 0) {
            free(*component);
            *component = text;
        } else if (strcmp(key, "File") == 0) {
            free(*details);
            *details = text;
        } else {
            free(text);
        }
        free(key);
    }

    if (!*component) {
        *component = strdup("(unknown)");
    }
    if (!*details) {
        *details = strdup("(unknown)");
    }
}

static gboolean handle_validator_message(GIOChannel*  source,
                                         GIOCondition condition,
                                         gpointer     data)
{
    dsme_log(LOG_DEBUG, "Activity on Validator socket");

    bool keep_listening = true;

    if (condition & G_IO_IN) {
        static struct msghdr msg;
        struct nlmsghdr*     nlh = 0;

        static bool msg_initialized = false;
        if (!msg_initialized) {
            static struct sockaddr_nl addr;

            memset(&addr, 0, sizeof(addr));
            nlh = (struct nlmsghdr*)malloc(NLMSG_SPACE(VALIDATOR_MAX_PAYLOAD));
            // TODO: check for NULL & free when done
            memset(nlh, 0, NLMSG_SPACE(VALIDATOR_MAX_PAYLOAD));

            static struct iovec iov;
            iov.iov_base = (void*)nlh;
            iov.iov_len  = NLMSG_SPACE(VALIDATOR_MAX_PAYLOAD);

            msg.msg_name    = (void*)&addr;
            msg.msg_namelen = sizeof(addr);
            msg.msg_iov     = &iov;
            msg.msg_iovlen  = 1;

            msg_initialized = true;
        }

        if (recvmsg(validator_fd, &msg, 0) == -1) {
            dsme_log(LOG_ERR, "Error receiving Validator message");
            // TODO: should we stop listening?
        } else {
            char* component;
            char* details;
            parse_validator_message(NLMSG_DATA(nlh), &component, &details);

            dsme_log(LOG_CRIT, "Security MALF: %s %s", component, details);

            go_to_malf(component, details);

            // NOTE: we leak component and details;
            // it is OK because we are entering MALF anyway
        }
    }
    if (condition & (G_IO_ERR | G_IO_HUP)) {
        dsme_log(LOG_ERR, "ERR or HUP on Validator socket");
        keep_listening = false;
    }

    if (!keep_listening) {
        stop_listening_to_validator();
    }

    return keep_listening;
}


static bool start_listening_to_validator(void)
{
    validator_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_VALIDATOR);
    if (validator_fd == -1) {
        dsme_log(LOG_ERR, "Validator socket: %s", strerror(errno));
        goto fail;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid    = getpid();
    addr.nl_groups = 1; // TODO: magic number: group mask for Validator

    if (bind(validator_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        dsme_log(LOG_ERR, "Validator socket bind: %s", strerror(errno));
        goto close_and_fail;
    }

    guint watch = 0;

    if (!(channel = g_io_channel_unix_new(validator_fd))) {
        goto close_and_fail;
    }
    watch = g_io_add_watch(channel,
                           (G_IO_IN | G_IO_ERR | G_IO_HUP),
                           handle_validator_message,
                           0);
    g_io_channel_unref(channel);
    if (!watch) {
        goto close_and_fail;
    }

    return true;


close_and_fail:
    close(validator_fd);
    validator_fd = -1;

fail:
    return false;
}

static void stop_listening_to_validator(void)
{
    if (validator_fd != -1) {
        dsme_log(LOG_DEBUG, "closing Validator socket");

        g_io_channel_set_close_on_unref(channel, FALSE);
        g_io_channel_unref(channel);
        channel = 0;

        close(validator_fd);
        validator_fd = -1;
    }
}


static void init_done_ind(const DsmeDbusMessage* ind)
{
    dsme_log(LOG_DEBUG, "base_boot_done; not listening to Validator");
    stop_listening_to_validator();
}

static bool bound = false;

static const dsme_dbus_signal_binding_t signals[] = {
    { init_done_ind, "com.nokia.startup.signal", "base_boot_done" },
    { 0, 0 }
};

DSME_HANDLER(DSM_MSGTYPE_DBUS_CONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, "validatorlistener: DBUS_CONNECT");
  dsme_dbus_bind_signals(&bound, signals);
}

DSME_HANDLER(DSM_MSGTYPE_DBUS_DISCONNECT, client, msg)
{
  dsme_log(LOG_DEBUG, "validatorlistener: DBUS_DISCONNECT");
  dsme_dbus_unbind_signals(&bound, signals);
}

module_fn_info_t message_handlers[] = {
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_CONNECT),
  DSME_HANDLER_BINDING(DSM_MSGTYPE_DBUS_DISCONNECT),
  { 0 }
};


void module_init(module_t* handle)
{
    dsme_log(LOG_DEBUG, "validatorlistener.so loaded");

    if (!start_listening_to_validator()) {
        dsme_log(LOG_CRIT, "failed to start listening to Validator");
        // TODO: what now?
    }
}

void module_fini(void)
{
    dsme_log(LOG_DEBUG, "validatorlistener.so unloaded");
}
