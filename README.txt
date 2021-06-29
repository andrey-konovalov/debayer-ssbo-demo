Building the demo (please ignore the build warning for now):
    make

Usage:
    ./debayer-ssbo-demo ../bayer.data debayer.data
Where ../bayer.data is the RAW8 bayer data copied from [1]

The demosaiced image is written to debayer.data, and the below command
can be used to convert it from RGBA into viewable pnm format:
    raw2rgbpnm -s 1920x1080 -f RGB32 debayer.data debayer.pnm

raw2rgbpnm can be built from sources at [2].

[1] https://github.com/NXPmicro/gtec-demo-framework/blob/master/DemoApps/OpenCL/SoftISP/Content/bayer.data
[2] git://git.retiisi.org.uk/~sailus/raw2rgbpnm.git
