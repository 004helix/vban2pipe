# VBAN receiver

This program receives audio data sent using [VB-Audio software](https://www.vb-audio.com/)
and send it to named pipe.

# Usage:

```
vban2pipe <port> <pipe> [exec-on-connect] [exec-on-disconnect]
```

# Example for pulseaudio:

Load pipe-source module
```
$ pactl load-module module-pipe-source source_name=pipe \
  file=/tmp/vban.input format=s16le rate=48000 channels=2
```

Start vban2pipe
```
$ vban2pipe 6980 /tmp/vban.input
```

Enable VBAN stream on sender side and you should see
```
$ vban2pipe 6980 /tmp/vban.input
[Stream1] stream connected from 172.16.0.2:59708, s16le, 48000 Hz, 2 channel(s)
[Stream1] stream online, primary
...

```

This program can receive several streams simultaneously. All streams should use
the same format and sample rate. Backup streams will be used to restore lost
packets after streams synchronization:
```
$ vban2pipe 6980 /tmp/vban.input
[Stream1] stream connected from 172.16.0.2:56503, s16le, 48000 Hz, 2 channel(s)
[Stream1] stream online, primary
[Stream2] stream connected from 172.16.0.2:56504, s16le, 48000 Hz, 2 channel(s)
[Stream2] stream online, offset -180480 samples
[Stream3] stream connected from 172.16.0.2:56505, s16le, 48000 Hz, 2 channel(s)
[Stream3] stream online, offset -435456 samples
...
```

If lost packets cannot be restored, vban2pipe reports lost output:
```
$ export DEBUG=1
$ vban2pipe 6980 /tmp/vban.input
...
[Stream2] expected 3490818, got 3490821: lost 3 packets
[Stream3] expected 3490818, got 3490821: lost 3 packets
<out> lost 256 samples
[Stream2] expected 3490843, got 3490845: lost 2 packets
[Stream3] expected 3490843, got 3490845: lost 2 packets
...

```

Output latency is only 2 packets in stream.
It is minimal latency by design, but you can
increase it in vban2pipe.c:
```
#define STREAM_TIMEOUT_MSEC 700
#define BUFFER_OUT_PACKETS 2    <-- HERE

```
