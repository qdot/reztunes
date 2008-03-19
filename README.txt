rezTunes 1.1
============

 This is a simple beat detection plugin for iTunes on OSX.  If you have a "Rez" 
trance vibrator(s) or workalike plugged in when you start iTunes, it should pulse
along in time with the music.  Unfortunately, the vibrator takes a little while to
spin up, so the implementation isn't as good as it could be.

 If you have a vibrator plugged in, the window will pulse with grey tones when
detecting beats.  If you don't have one plugged in, the window will pulse red
instead.

 If you want to tweak the beat detection code to suit a particular style of music,
have a look at the defines at the top of rezTunes.c - they're all documented.

 Thanks to - the team at Apple who contributed sample code for USB and iTunes
visualisations, http://cathand.org/ and Sasha and Nick who contributed ideas on
band processing and mathematics in turn.

 rezTunes is copyright, 2007, Bryn Davies, but see the License below for details.

 Code altered for cross platform compatibility by Kyle Machulis, http://www.nonpolynomial.com

Installation
============

 Once you have a build of the bundle file, either from building it yourself or
downloading it from elsewhere, drop it in ~/Library/iTunes/iTunes Plugins/

 It should now be selectable from the visualisations menu.  The vibrator will
only run while the saver is active, providing you with at least a rudimentary
boss-key ( Apple-T ).

License
=======

 This work is licensed under the Creative Commons Attribution 3.0 license, 
detailed at http://creativecommons.org/licenses/by/3.0/ .  You are free to 
share and remix this sourcecode, providing it is attributed correctly. See
LICENSE.txt for more information.
