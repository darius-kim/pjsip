pjsip 2.1.0 openh264/libyuv support version.
this repository was not pjsip official project.

this source distributed under pjsip, openh264, libyuv licences

license detail at

pjsip : http://www.pjsip.org/licensing.htm
openh264 : http://www.openh264.org/faq.html
libyuv : https://code.google.com/p/libyuv/source/browse/trunk/LICENSE

installation

1. clone repository
2. download sdl2 binary from http://www.libsdl.org/hg.php
3. extract sdl2 to git working directory
4. open pjproject-2.1.0/pjproject-vs9.sln
5. build pjproject
6. check libyuv.lib at <working directory>/libyuv
7. copy sdl2.dll to pjproject-2.1.0/pjsip-apps/bin
8. copy welsdec.dll/welsenc.dll/welsvp.dll to pjproject-2.1.0/pjsip-apps/bin