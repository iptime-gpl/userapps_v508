#!/bin/sh
aipc_util -C IOCTL_CTRL_2CPU_SEND -F CTRL_2CPU_SEND_START
aipc_util -C IOCTL_CTRL_2CPU_POLL -F CTRL_2CPU_POLL_START