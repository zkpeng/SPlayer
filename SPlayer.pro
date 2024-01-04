TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

#win32 {
#INCLUDEPATH += $$PWD/ffmpeg-4.2.1-win32-dev/include
#LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avformat.lib   \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avcodec.lib    \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avdevice.lib   \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avfilter.lib   \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/avutil.lib     \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/postproc.lib   \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/swresample.lib \
#        $$PWD/ffmpeg-4.2.1-win32-dev/lib/swscale.lib
#}

win32{
INCLUDEPATH += $$PWD/ffmpeg-4.2.1-win32-dev/include
INCLUDEPATH += $$PWD/SDL2/include

LIBS += -L$$PWD/ffmpeg-4.2.1-win32-dev/lib -lavformat
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avcodec.lib
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avdevice.lib
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avfilter.lib
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/avutil.lib
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/swresample.lib
LIBS += $$PWD/ffmpeg-4.2.1-win32-dev/lib/swscale.lib

LIBS += -L$$PWD/SDL2/lib/x86 -lSDL2
LIBS += -L$$PWD/SDL2/lib/x88 -lOle32

#LIBS += $$PWD/SDL2/lib/x86/SDL2main.lib \
#     += $$PWD/SDL2/lib/x86/SDL2test.lib
}

SOURCES += \
    main.c

