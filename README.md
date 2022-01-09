timber - tree-based Wayland compositor
======================================

[![GitLab Build Status](https://gitlab.com/timberwm/timber/badges/main/pipeline.svg)](https://gitlab.com/timberwm/timber/-/commits/main)
[![Coverity Status](https://scan.coverity.com/projects/17917/badge.svg)](https://scan.coverity.com/projects/timber)

The timber window manager is a simple Wayland compositor which is
heavily inspired by dwm[1] and bspwm[2]. As with bspwm, timber
uses binary trees to keep track of clients and manage their
layouting.

Requirements
------------

In order to build timber you need to have the following dependencies installed:

    - meson >= 0.56.2
    - pixman
    - wayland-protocol
    - wlroots >= 0.11.0
    - xkbcommon

Installation
------------

To build and install timber, you can execute the following
commands in the timber directory:

    $ meson build .
    $ ninja -C build install

By default, the timber executable will be installed into the
/usr/local namespace. You can change this by setting the DESTDIR
environment variable when executing ninja.

Running
-------

You can start timber by simply executing it:

    $ exec timber

It will then use the next available Wayland socket in the
`XDG_RUNTIME_DIR`. The directory must exist before timber is
executed.

In case you want to start timber with an initialization script,
you need to set the `TMBR_CONFIG_PATH` variable:

    $ TMBR_CONFIG_PATH=/path/to/timberrc exec timber

When timber is running, you can control it by using the commands
provided by timber:

    $ timber client_focus next

You can execute `timber -h` to get usage information.

[1]: https://dwm.suckless.org
[2]: https://github.com/baskerville/bspwm
