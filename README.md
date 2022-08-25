# GNU-Linux_AudioRTDSP2
An user-level runtime code for GNU-Linux based system.
This code performs Audio Real-Time Digital Signal Processing, more specifically, it runs a (roughly calculated) differential equation on the audio signal.

This code uses headerless (.raw) audio files as an input audio signal. It's meant to be used playing a stereo, 44.1 kHz 16bit audio file.
Different audio signal parameters might not work properly on this code.

This code requires the asoundlib headers and build resources. 
In Debian based system, these resources can be installed using command sudo apt-get install libasound2-dev

When compiling, 2 APIs must be manualy linked: "asound" and "pthread"
Example: g++ main.cpp -lpthread -lasound -o <executable name>
  
I made this code just for fun. I'm not a professional software developer. Don't expect professional performance from it.
  
Author: Rafael Sabe
Email: rafaelmsabe@gmail.com
