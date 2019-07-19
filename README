timber - tree-based window manager
==================================

The timber window manager is a simple tiling window manager that
is heavily inspired by dwm[1] and bspwm[2]. As with bspwm, timber
uses binary trees to keep track of clients and manage their
layouting. As timber strives for simplicity, it does not do any
input handling. Instead, all commands need to be written into a
named pipe via a hotkey manager like sxhkd [3].

Requirements
------------

In order to build timber you need to have the xcb, xcb-aux and
xcb-ewmh header and library files installed. Furthermore, timber
makes use of the meson build system.

Installation
------------

If you desire, you can modify the file "src/config.h.in" to match
your own needs. Besides the path where the control socket shall
be created, it also contains options to modify the border width
and color.

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

In case you want to start it on a specific X display, you can set
the DISPLAY variable:

    $ DISPLAY=:1 exec timber

When timber is running, you can control it by using the commands
provided by timber:

    $ timber client_focus next

You can execute `timber -h` to get usage information. Usually,
you would configure a hotkey manager like sxhkd to bind these
commands to hotkeys.


[1]: https://dwm.suckless.org
[2]: https://github.com/baskerville/bspwm
[3]: https://github.com/baskerville/sxhkd
