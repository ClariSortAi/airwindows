# FFmpeg overlay port

This folder is a copy of `ports/ffmpeg` from vcpkg at commit `e456309491fd875487bf02b8a0dca76c1b1b00cc` (FFmpeg 9.0.2), with one change in `portfile.cmake`.

The upstream port builds every codec FFmpeg has and offers no switch to limit them. AirPlay Receiver opens four decoders, so the Windows branch of the portfile now passes these options to FFmpeg's `configure`:

```
--enable-d3d11va
--disable-everything
--enable-decoder=h264,hevc,aac,alac
--enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2
```

The app uses the two `_d3d11va2` accelerators. The two without the `2` are enabled only because FFmpeg's makefile compiles the shared source file under their names, and the link fails without them.

The upstream line also enabled D3D12VA, DXVA2 and Media Foundation, which the app does not use. They are left out.

The result is an LGPL-2.1-or-later build with no GPL or nonfree parts. `vcpkg-configuration.json` in the project root points vcpkg at this folder.

To update FFmpeg, copy the port again from a newer vcpkg commit and reapply the change. Search `portfile.cmake` for "AirPlay Receiver overlay" to find it.

The port files are from the vcpkg project and are MIT licensed.
