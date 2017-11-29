# Pulseaudio VBAN receiver

This program receives audio data sent using [VB-Audio software](https://www.vb-audio.com/)
and resend it to named pipe.

# Usage:

```
pulse-vban2pipe <port> <pulseaudio-pipe> [exec-on-connect] [exec-on-disconnect]
```

# Example:

load pipe-source module
```
$ pactl load-module module-pipe-source source_name=pipe \
  file=/tmp/vban.input format=s16le rate=48000 channels=2
```

start pulse-vban2pipe
```
$ pulse-vban2pipe 6980 /tmp/vban.input
```

enable VBAN stream on sender side and you should see
```
$ pulse-vban2pipe 6980 /tmp/vban.input
[Stream1] stream connected from 172.16.0.2:59708, s16le, 48000 Hz, 2 channel(s)
[Stream1] stream online, primary
...

```

This program can receive several streams simultaneously. All streams should use
the same format and sample rate. Backup streams will used to restore lost
packets after streams synchronization:
```
$ pulse-vban2pipe 6980 /tmp/vban.input
[Stream1] stream connected from 172.16.0.2:56503, s16le, 48000 Hz, 2 channel(s)
[Stream1] stream online, primary
[Stream2] stream connected from 172.16.0.2:56504, s16le, 48000 Hz, 2 channel(s)
[Stream2] stream online, offset -180480 samples
[Stream3] stream connected from 172.16.0.2:56505, s16le, 48000 Hz, 2 channel(s)
[Stream3] stream online, offset -435456 samples
[Stream1] expected 3512424, got 3512425: lost 1 packet(s)
[Stream2] expected 3512422, got 3512425: lost 3 packet(s)
[Stream3] expected 6103348, got 6103349: lost 1 packet(s)
...
```

If lost packets cannot be restored, pulse-vban2pipe reports lost output:
```
...
[Stream2] expected 3490818, got 3490821: lost 3 packet(s)
[Stream3] expected 3490818, got 3490821: lost 3 packet(s)
<out> lost 256 samples
[Stream2] expected 3490843, got 3490845: lost 2 packet(s)
[Stream3] expected 3490843, got 3490845: lost 2 packet(s)
...

```
