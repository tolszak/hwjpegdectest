# hwjpegdectest
Test program to check how fast jpegs can be decoded on odroidc2 using hw jpeg decoder.



prerequisites:
 sudo apt-get install aml-libs and libasound2-dev

to build:
 g++ -std=c++11 -o hwjpegdecodertest main.cpp -L/usr/lib/aml_libs/ -lamcodec -lamadec -lamavutils -lasound -lpthread
