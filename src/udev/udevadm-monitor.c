/*
 * Copyright (C) 2004-2010 Kay Sievers <kay.sievers@vrfy.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <linux/types.h>
#include <linux/netlink.h>

#include "udev.h"

static bool udev_exit;

static void sig_handler(int signum)
{
        if (signum == SIGINT || signum == SIGTERM)
                udev_exit = true;
}

static void print_device(struct udev_device *device, const char *source, int prop)
{
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("%-6s[%llu.%06u] %-8s %s (%s)\n",
               source,
               (unsigned long long) ts.tv_sec, (unsigned int) ts.tv_nsec/1000,
               udev_device_get_action(device),
               udev_device_get_devpath(device),
               udev_device_get_subsystem(device));
        if (prop) {
                struct udev_list_entry *list_entry;

                udev_list_entry_foreach(list_entry, udev_device_get_properties_list_entry(device))
                        printf("%s=%s\n",
                               udev_list_entry_get_name(list_entry),
                               udev_list_entry_get_value(list_entry));
                printf("\n");
        }
}

static int adm_monitor(struct udev *udev, int argc, char *argv[])
{
        struct sigaction act;
        sigset_t mask;
        int option;
        bool prop = false;
        bool print_kernel = false;
        bool print_udev = false;
        struct udev_list subsystem_match_list;
        struct udev_list tag_match_list;
        struct udev_monitor *udev_monitor = NULL;
        struct udev_monitor *kernel_monitor = NULL;
        int fd_ep = -1;
        int fd_kernel = -1, fd_udev = -1;
        struct epoll_event ep_kernel, ep_udev;
        int rc = 0;

        static const struct option options[] = {
                { "property", no_argument, NULL, 'p' },
                { "environment", no_argument, NULL, 'e' },
                { "kernel", no_argument, NULL, 'k' },
                { "udev", no_argument, NULL, 'u' },
                { "subsystem-match", required_argument, NULL, 's' },
                { "tag-match", required_argument, NULL, 't' },
                { "help", no_argument, NULL, 'h' },
                {}
        };

        udev_list_init(udev, &subsystem_match_list, true);
        udev_list_init(udev, &tag_match_list, true);

        for (;;) {
                option = getopt_long(argc, argv, "pekus:t:h", options, NULL);
                if (option == -1)
                        break;

                switch (option) {
                case 'p':
                case 'e':
                        prop = true;
                        break;
                case 'k':
                        print_kernel = true;
                        break;
                case 'u':
                        print_udev = true;
                        break;
                case 's':
                        {
                                char subsys[UTIL_NAME_SIZE];
                                char *devtype;

                                util_strscpy(subsys, sizeof(subsys), optarg);
                                devtype = strchr(subsys, '/');
                                if (devtype != NULL) {
                                        devtype[0] = '\0';
                                        devtype++;
                                }
                                udev_list_entry_add(&subsystem_match_list, subsys, devtype);
                                break;
                        }
                case 't':
                        udev_list_entry_add(&tag_match_list, optarg, NULL);
                        break;
                case 'h':
                        printf("Usage: udevadm monitor [--property] [--kernel] [--udev] [--help]\n"
                               "  --property                              print the event properties\n"
                               "  --kernel                                print kernel uevents\n"
                               "  --udev                                  print udev events\n"
                               "  --subsystem-match=<subsystem[/devtype]> filter events by subsystem\n"
                               "  --tag-match=<tag>                       filter events by tag\n"
                               "  --help\n\n");
                        goto out;
                default:
                        rc = 1;
                        goto out;
                }
        }

        if (!print_kernel && !print_udev) {
                print_kernel = true;
                print_udev = true;
        }

        /* set signal handlers */
        memset(&act, 0x00, sizeof(struct sigaction));
        act.sa_handler = sig_handler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_RESTART;
        sigaction(SIGINT, &act, NULL);
        sigaction(SIGTERM, &act, NULL);
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        fd_ep = epoll_create1(EPOLL_CLOEXEC);
        if (fd_ep < 0) {
                log_error("error creating epoll fd: %m\n");
                goto out;
        }

        printf("monitor will print the received events for:\n");
        if (print_udev) {
                struct udev_list_entry *entry;

                udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
                if (udev_monitor == NULL) {
                        fprintf(stderr, "error: unable to create netlink socket\n");
                        rc = 1;
                        goto out;
                }
                udev_monitor_set_receive_buffer_size(udev_monitor, 128*1024*1024);
                fd_udev = udev_monitor_get_fd(udev_monitor);

                udev_list_entry_foreach(entry, udev_list_get_entry(&subsystem_match_list)) {
                        const char *subsys = udev_list_entry_get_name(entry);
                        const char *devtype = udev_list_entry_get_value(entry);

                        if (udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, subsys, devtype) < 0)
                                fprintf(stderr, "error: unable to apply subsystem filter '%s'\n", subsys);
                }

                udev_list_entry_foreach(entry, udev_list_get_entry(&tag_match_list)) {
                        const char *tag = udev_list_entry_get_name(entry);

                        if (udev_monitor_filter_add_match_tag(udev_monitor, tag) < 0)
                                fprintf(stderr, "error: unable to apply tag filter '%s'\n", tag);
                }

                if (udev_monitor_enable_receiving(udev_monitor) < 0) {
                        fprintf(stderr, "error: unable to subscribe to udev events\n");
                        rc = 2;
                        goto out;
                }

                memset(&ep_udev, 0, sizeof(struct epoll_event));
                ep_udev.events = EPOLLIN;
                ep_udev.data.fd = fd_udev;
                if (epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_udev, &ep_udev) < 0) {
                        log_error("fail to add fd to epoll: %m\n");
                        goto out;
                }

                printf("UDEV - the event which udev sends out after rule processing\n");
        }

        if (print_kernel) {
                struct udev_list_entry *entry;

                kernel_monitor = udev_monitor_new_from_netlink(udev, "kernel");
                if (kernel_monitor == NULL) {
                        fprintf(stderr, "error: unable to create netlink socket\n");
                        rc = 3;
                        goto out;
                }
                udev_monitor_set_receive_buffer_size(kernel_monitor, 128*1024*1024);
                fd_kernel = udev_monitor_get_fd(kernel_monitor);

                udev_list_entry_foreach(entry, udev_list_get_entry(&subsystem_match_list)) {
                        const char *subsys = udev_list_entry_get_name(entry);

                        if (udev_monitor_filter_add_match_subsystem_devtype(kernel_monitor, subsys, NULL) < 0)
                                fprintf(stderr, "error: unable to apply subsystem filter '%s'\n", subsys);
                }

                if (udev_monitor_enable_receiving(kernel_monitor) < 0) {
                        fprintf(stderr, "error: unable to subscribe to kernel events\n");
                        rc = 4;
                        goto out;
                }

                memset(&ep_kernel, 0, sizeof(struct epoll_event));
                ep_kernel.events = EPOLLIN;
                ep_kernel.data.fd = fd_kernel;
                if (epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_kernel, &ep_kernel) < 0) {
                        log_error("fail to add fd to epoll: %m\n");
                        goto out;
                }

                printf("KERNEL - the kernel uevent\n");
        }
        printf("\n");

        while (!udev_exit) {
                int fdcount;
                struct epoll_event ev[4];
                int i;

                fdcount = epoll_wait(fd_ep, ev, ELEMENTSOF(ev), -1);
                if (fdcount < 0) {
                        if (errno != EINTR)
                                fprintf(stderr, "error receiving uevent message: %m\n");
                        continue;
                }

                for (i = 0; i < fdcount; i++) {
                        if (ev[i].data.fd == fd_kernel && ev[i].events & EPOLLIN) {
                                struct udev_device *device;

                                device = udev_monitor_receive_device(kernel_monitor);
                                if (device == NULL)
                                        continue;
                                print_device(device, "KERNEL", prop);
                                udev_device_unref(device);
                        } else if (ev[i].data.fd == fd_udev && ev[i].events & EPOLLIN) {
                                struct udev_device *device;

                                device = udev_monitor_receive_device(udev_monitor);
                                if (device == NULL)
                                        continue;
                                print_device(device, "UDEV", prop);
                                udev_device_unref(device);
                        }
                }
        }
out:
        if (fd_ep >= 0)
                close(fd_ep);
        udev_monitor_unref(udev_monitor);
        udev_monitor_unref(kernel_monitor);
        udev_list_cleanup(&subsystem_match_list);
        udev_list_cleanup(&tag_match_list);
        return rc;
}

const struct udevadm_cmd udevadm_monitor = {
        .name = "monitor",
        .cmd = adm_monitor,
        .help = "listen to kernel and udev events",
};
