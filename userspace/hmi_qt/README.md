# RK3568 Qt Wayland HMI

This is the B2 HMI consumer for the AI runtime. It is intentionally a manual
program: it adds no init script and does not change S90, S91, the AI process,
the bridge, or `safety_alarmd`.

## Ownership

The program reads the AI runtime state and JPEG fallback frame:

```text
/opt/rk3568_yolov5_demo/hmi_runtime/ai_runtime.json
/opt/rk3568_yolov5_demo/hmi_runtime/preview/latest.jpg
```

For the primary preview path it connects to the local H.264 publisher:

```text
/opt/rk3568_yolov5_demo/hmi_runtime/h264.sock
```

The HMI is the socket client. It receives H.264 configuration/access-unit
messages only; it does not send control commands. It does not open
`/dev/video9`, write runtime files, or access GPIO, PWM, I2C, UART, MQTT, or
SQLite.

`ai_runtime.json` is read first. The AI publishes
`preview_frame_path` as `hmi_runtime/preview/latest.jpg`, relative to the AI
working directory. This HMI resolves that relative path against the parent of
`--runtime-dir`, producing the fixed JPEG path above.

The live preview path is:

```text
AF_UNIX SOCK_SEQPACKET -> appsrc -> h264parse -> mppvideodec -> waylandsink
```

On connection or reconnection, the HMI waits for H.264 parameter sets and an
IDR access unit before submitting video to the pipeline. Socket, decode, or
pipeline failure keeps the JPEG path available and marks the UI as degraded.

## Degraded states

The UI does not display a safe/normal status when JSON is missing, malformed,
or older than 3 seconds. It also marks the page degraded when `camera_state` is
not `online`, `ai_state` is not `running`, JPEG loading fails, the preview
sequence does not change for 3 seconds, or H.264 is unavailable. When AI
stops, the last decoded frame may remain visible but the UI explicitly marks
the preview as expired and does not present the alarm state as safe.

Temperature, humidity, illuminance, radar details, and actual actuator feedback
are intentionally absent: B1 does not publish a unified source for them yet.

## Cross build

Run on the Ubuntu development host after copying this directory to
`~/rk3568_project/userspace/hmi_qt`:

```sh
cd ~/rk3568_project || exit 1
cmake -S userspace/hmi_qt -B userspace/hmi_qt/build-toolchain \
  -DCMAKE_TOOLCHAIN_FILE=userspace/ai_yolov5/toolchain-rk3568.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build userspace/hmi_qt/build-toolchain --target rk3568_hmi -j2
file userspace/hmi_qt/build-toolchain/rk3568_hmi
```

The target must be an ARM aarch64 ELF and must dynamically link against the
existing Qt5 Widgets and GStreamer libraries in the Buildroot sysroot.

## Manual deployment and board run

Install the binary as `/opt/rk3568_hmi/rk3568_hmi`, separate from the root-only
AI directory. The HMI reads `/run/industrial_safety/system_state.json`; it still
uses `--runtime-dir` only for H.264 `h264.sock` and the JPEG fallback media files.
Start `/etc/init.d/S92state_snapshotd` after `S90safety_alarmd` and before HMI.

The board run is manual. Weston and `/run/wayland-0` must already be available:

```sh
QT_QPA_PLATFORM=wayland \
WAYLAND_DISPLAY=wayland-0 \
XDG_RUNTIME_DIR=/run \
/opt/rk3568_hmi/rk3568_hmi \
  --runtime-dir /opt/rk3568_yolov5_demo/hmi_runtime
```

Board verification completed on the RK3568 MIPI display:

- H.264 preview is shown inside the Qt preview area without touching the panel.
- The blue ROI and green detection overlays are visible in the H.264 picture.
- H.264 encoding is about 30 FPS with no encode failures in the verified run.
- Stopping AI leaves the HMI and Weston alive and shows a degraded expired-preview state.
- Restarting AI reconnects the existing HMI and restores H.264 without reopening `/dev/video9` from HMI.
