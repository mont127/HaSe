# HaSe: CheeseBridge Vulkan Architecture Notes


## Status

CheeseBridge is an experimental research component for MacNCheese.

It is not a production backend, not a replacement for the current MacNCheese Wine path, and not intended for real game support yet. The first goal is to prove that a Linux guest can send Vulkan-like work to a macOS host process and receive correct responses.

## Phase 1 Demo Build

The default CMake build creates the first fake bridge prototype:

```sh
cmake -S . -B build
cmake --build build
```

Run the fake host in one terminal:

```sh
build/demo/cheesebridge_fake_host tcp:127.0.0.1:43210
```

Run the guest demo in another terminal:

```sh
build/demo/cheesebridge_guest_demo tcp:127.0.0.1:43210
```

The guest sends HELLO, CAPABILITY_QUERY, CREATE_INSTANCE, ENUMERATE_PHYSICAL_DEVICES, CREATE_DEVICE, CREATE_BUFFER, QUEUE_SUBMIT, and PRESENT messages. The host logs each request and replies with placeholder ids. This phase intentionally does not require MoltenVK, Metal, Vulkan headers, Wine, DXVK, or a VM.

## Phase 2 Vulkan ICD Stub

The Linux guest ICD can now run as a local stub when no macOS host endpoint is configured. This is the Phase 2 path for loader discovery and `vulkaninfo` smoke tests inside the Linux guest:

```sh
cmake -S . -B build-guest -DCHEESEBRIDGE_BUILD_GUEST=ON -DCHEESEBRIDGE_BUILD_DEMO=OFF
cmake --build build-guest
VK_DRIVER_FILES=$PWD/build-guest/guest/cheesebridge_icd.dev.json CHEESEBRIDGE_LOG=info vulkaninfo --summary
```

By default the ICD uses the local stub backend unless `CHEESEBRIDGE_HOST` is set. To force the network forwarding path later:

```sh
CHEESEBRIDGE_STUB=0 CHEESEBRIDGE_HOST=tcp:127.0.0.1:43210 vulkaninfo --summary
```

The stub reports one integrated GPU named `CheeseBridge Vulkan ICD Stub` and answers the basic instance, physical device, memory, queue-family, and device creation queries expected during early loader probing.

## Overview

HaSe is an experimental MacNCheese backend designed for a future where relying on Rosetta 2 may not be enough.

Instead of running the full compatibility stack directly through macOS Wine, HaSe uses a small ARM64 Linux environment. Inside that guest, FEX, Wine, DXVK, and VKD3D-Proton can run in a Proton-like stack.

The hard part is graphics acceleration.

CheeseBridge (still a concept (gonna be really complicated making this but possible)) exists to make the Linux guest believe it has a Vulkan-capable GPU while the macOS host performs the actual rendering through MoltenVK or a native Metal backend.

## High-Level Stack

```text
Windows x86_64 game
        ↓
FEX-Emu
        ↓
Wine / Proton-style runtime
        ↓
DXVK / VKD3D-Proton
        ↓
Guest Vulkan loader
        ↓
CheeseBridge Vulkan ICD
        ↓
CheeseBridge guest protocol layer
        ↓
VM communication layer
        ↓
CheeseBridge host on macOS
        ↓
MoltenVK or custom Metal backend
        ↓
Metal
        ↓
Apple GPU
```

## #VULKANMATTERS (Why vulkan is an easy option)

DXVK and VKD3D-Proton already translate DirectX workloads into Vulkan.

That means HaSe does not need to start by implementing DirectX handling. Wine, DXVK, and VKD3D-Proton already cover that layer.

The main problem becomes this:

How can Vulkan inside a Linux guest render using the macOS host GPU?

CheeseBridge is the proposed answer to that problem.

## What CheeseBridge Is (A concept)

CheeseBridge is a virtual Vulkan bridge with two sides.

The guest side runs inside the Linux VM. It presents itself as a Vulkan driver or Vulkan ICD, receives Vulkan calls from DXVK or VKD3D-Proton, serializes those calls, and sends them across the VM boundary.

The host side runs on macOS. It receives the serialized commands, recreates or translates the requested work, executes it through MoltenVK or Metal, and presents the final frame on macOS.

## Simplified Render Flow (Boooooriiiiinnggg)

```text
Game makes a DirectX call
        ↓
DXVK converts it to Vulkan
        ↓
Guest Vulkan ICD receives the Vulkan command
        ↓
CheeseBridge serializes the command
        ↓
Command crosses the VM boundary
        ↓
macOS host receives the command
        ↓
Host executes through MoltenVK or Metal
        ↓
Frame appears on screen
```

## Important stuff and design ig

MoltenVK by itself is not enough.

MoltenVK is a Vulkan implementation for macOS that runs over Metal. The Linux guest cannot directly use MoltenVK because MoltenVK runs on the macOS host, not inside the Linux VM.

CheeseBridge needs to provide the missing path:

```text
Guest Vulkan API
        ↓
Bridge protocol
        ↓
Host Vulkan or Metal backend
```

The concept is similar to Vulkan forwarding designs such as Venus and virtio-gpu, where commands from a guest are forwarded to a renderer on the host.

## Main Components (Architecture stuff )

## Vulkan Loader

Inside Linux, applications usually do not call the GPU driver directly. They call the Vulkan loader first.

DXVK and VKD3D-Proton call Vulkan functions through the loader. The loader then discovers and loads an ICD.

## Vulkan ICD 

ICD means Installable Client Driver.

For HaSe, CheeseBridge needs a custom Vulkan ICD inside the Linux guest. This ICD is responsible for exposing Vulkan entry points and connecting those calls to the CheeseBridge guest protocol layer.

Early target entry points may include:

```text
vkGetInstanceProcAddr
vkGetDeviceProcAddr
vkCreateInstance
vkEnumeratePhysicalDevices
vkCreateDevice
vkCreateBuffer
vkCreateImage
vkQueueSubmit
vkQueuePresentKHR
```

The first ICD does not need full Vulkan support. It can start as a stub that loads through the Vulkan loader, logs calls, connects to the host, and returns controlled fake device information.

## CheeseBridge Protocol (Complex stuff)

The protocol is the communication format between the Linux guest and the macOS host.

Early prototypes can use JSON or simple structs (this is what macndcheese uses for backend communication with the frontend app). Later versions should move to a binary protocol with shared memory for performance.

Initial message types:

```text
HELLO
HELLO_REPLY
CAPABILITY_QUERY
CAPABILITY_REPLY
CREATE_INSTANCE
CREATE_INSTANCE_REPLY
ENUMERATE_PHYSICAL_DEVICES
PHYSICAL_DEVICES_REPLY
CREATE_DEVICE
CREATE_DEVICE_REPLY
CREATE_BUFFER
CREATE_BUFFER_REPLY
CREATE_IMAGE
CREATE_IMAGE_REPLY
CREATE_SHADER_MODULE
QUEUE_SUBMIT
QUEUE_SUBMIT_REPLY
PRESENT
PRESENT_REPLY
DESTROY_RESOURCE
```

## Host Renderer

The host renderer runs on macOS and executes work requested by the guest.

There are three possible backend paths.

Option A is MoltenVK. Guest Vulkan commands are reconstructed on the host and passed into MoltenVK. This is the best first target because shader translation and Vulkan-to-Metal behavior already exist there.

Option B is a custom Metal backend. This could be faster later, but it is much harder because it requires direct handling of Vulkan concepts, shader translation, pipeline state, synchronization, memory, and presentation.

Option C is a hybrid model. CheeseBridge starts with MoltenVK and later replaces selected performance-critical paths with native Metal code.

The recommended first backend is MoltenVK.

## Memory Model(Even more complex (still possible))

Graphics forwarding is not only about commands. It is also about memory.

CheeseBridge must handle guest memory, host memory, GPU-visible memory, and the VM boundary between them.

Required areas include:

```text
Buffer creation
Image and texture creation
Memory allocation
Memory mapping
Resource lifetime
Host and guest copies
Shared memory
GPU fences
Semaphores
Synchronization objects
```

Apple Silicon unified memory helps, but it does not remove the VM boundary. Guest memory and host memory still need to be mapped, copied, or shared correctly.

## Synchronization (#WELOVEVULKAN)

Vulkan synchronization is strict and central to correctness.

CheeseBridge will eventually need support for:

```text
Fences
Binary semaphores
Timeline semaphores
Queue submissions
Image layout transitions
Swapchain timing
Present synchronization
```

Broken synchronization can create failures that are difficult to debug. Symptoms may include flickering, corrupted frames, random freezes, crashes, or rendering bugs that only appear sometimes.

## Shader Handling

DXVK and VKD3D-Proton generate shaders for Vulkan.

CheeseBridge must pass shader modules to the macOS host. If the host backend uses MoltenVK, MoltenVK can handle the Vulkan shader path toward Metal execution.

A custom Metal backend would make shader handling much harder and should not be part of the first implementation.

## Swapchain and Presentation 

Games render to a Vulkan swapchain.

Inside the Linux guest, DXVK expects a Vulkan swapchain. The final window, however, must exist on macOS.

A possible presentation model:

```text
Guest creates a fake Vulkan swapchain
        ↓
Guest renders to virtual swapchain images
        ↓
Host maps or copies those images into Metal textures
        ↓
Host presents through a macOS window owned by MacNCheese
```

Presentation should be treated as a separate milestone. It is not required for the first fake bridge prototype.

## Transport Layer

CheeseBridge needs a communication layer between the Linux guest and the macOS host.

The first implementation should use the simplest transport that proves the architecture. Performance can come later.

Possible transports:

```text
TCP localhost socket (My idea)
Unix socket
virtio-vsock
Shared memory
Custom virtio device
```

Recommended path:

```text
Start with sockets
Add structured protocol handling
Move high-volume data to shared memory
Consider virtio-vsock or a custom virtio device later
```

## Development Roadmap

 
### HaSe Windows Management

HaSe is the runtime manager. CheeseBridge is only the graphics bridge.

A HaSe bottle should be treated as a managed Linux bottle. It contains the Linux userspace, runtime configuration, FEX configuration, Steam or Proton installation, Wine prefix data, Vulkan ICD configuration, shared folders, and per-bottle launch metadata.

The first user-visible goal is not a full Linux desktop. The goal is a CrossOver-style experience where Steam and games appear as normal macOS windows while the Linux environment stays hidden.

The technical requirement is still a Linux kernel. macOS cannot run Linux binaries or Linux containers directly without a Linux kernel underneath. A Docker-style design does not remove this requirement on macOS, because Docker Desktop also runs Linux containers inside a hidden Linux VM.

The first backend should use a small hidden Linux VM. Lima is a good prototype backend because it can manage Linux VM creation, boot, SSH access, file sharing, and port forwarding with less custom code. HaSe can later replace Lima with a native backend built on Apple Virtualization.framework, QEMU, or a custom VM manager.

The VM should not boot into GNOME, KDE, or a normal desktop session. It should boot into a minimal graphical session with only the services needed for Steam, launchers, input, audio, file access, and window tracking.

A useful first layout is:
```text
HaSe bottle
    rootfs or VM image
    home directory
    Steam data
    Proton runtime
    Wine prefixes
    FEX rootfs and configuration
    Vulkan loader configuration
    CheeseBridge ICD
    shared folders
    launch metadata
    window metadata
```
The macOS side should own the visible user experience. HaSe should create the macOS windows, manage their position and size, start and stop the Linux runtime, and connect each visible window to the correct Linux window or game swapchain.

Steam should be allowed to appear. Users expect Steam login, library management, downloads, cloud saves, controller settings, game properties, and Proton selection. HaSe should not require users to launch games only from a custom HaSe library. The correct model is that Steam can appear as a normal macOS window, and games launched from Steam can open as their own macOS windows.

## First Display Model

The simplest first implementation can use a cropped framebuffer model.
```text
Linux graphical session
        ↓
Black background
        ↓
Steam opens a normal Linux window
        ↓
HaSe tracks the Steam window rectangle
        ↓
HaSe crops that region from the VM display
        ↓
HaSe presents the crop inside a macOS NSWindow
```
The same idea can be used for child windows:
```text
Steam login window
Steam main window
Game launcher
Game window
Popup menu
File picker
Settings dialog
```
Each Linux window can become a separate macOS window by tracking its geometry and cropping the correct region from the VM framebuffer. This gives a rootless-window feel without building a full Wayland-to-Cocoa compositor in the first version.

The Linux background should be black or otherwise neutral. No taskbar, panel, dock, wallpaper, desktop icons, or visible system UI should be shown. If the cropped region is correct, the user should only see the selected app window.

## Input Mapping
```text
Input must be translated back into Linux coordinates.

macOS window receives mouse event at local position
        ↓
HaSe converts local position to Linux display coordinates
        ↓
HaSe sends mouse event into the VM
        ↓
Linux app receives the event as if it happened inside its own window
```
Keyboard input needs the same treatment. HaSe should forward key down, key up, text input, modifier state, focus changes, and shortcut handling. Some shortcuts should remain macOS shortcuts. Others must be delivered to Linux. This needs explicit policy because games, Steam, and launchers expect different behavior.

## Window Resizing
```text
Window resizing should be handled in both directions.

Linux app resizes itself
        ↓
HaSe resizes the macOS window
macOS user resizes the window
        ↓
HaSe requests a Linux window resize
        ↓
Linux app redraws
        ↓
HaSe updates the crop
```
Menus, dropdowns, and tooltips are important. Many toolkits create them as separate temporary windows. HaSe should detect those windows and either attach them to the parent macOS window or expose them as borderless child NSWindows positioned above the parent.

### Game Rendering Path

Fullscreen games need a separate path. A game that uses Vulkan through DXVK or VKD3D-Proton should eventually present through CheeseBridge instead of the cropped VM framebuffer. The cropped framebuffer path is useful for Steam, launchers, installers, login dialogs, and non-accelerated UI. The CheeseBridge path is for high-performance game rendering.

The long-term display model should have two paths.
```text
Steam and launcher UI
        ↓
Linux window system
        ↓
HaSe window management
        ↓
macOS NSWindow
Game rendering
        ↓
DXVK or VKD3D-Proton
        ↓
Vulkan
        ↓
CheeseBridge
        ↓
macOS Metal window
```
The cropped framebuffer path can be the first working version. A later version can replace parts of it with a real rootless window bridge that maps Wayland or X11 surfaces more directly into macOS windows.
For the first prototype, the Linux side can use X11 with a tiny window manager because X11 window geometry and reparenting behavior are easier to inspect. A later Wayland path can use a small custom compositor if needed. The compositor or window manager should expose window IDs, titles, positions, sizes, focus state, stacking order, and parent-child relationships to HaSe.

## macOS Window Table

HaSe should keep a window table on the macOS side.
```text
linux_window_id
macos_window_id
title
process_id
x
y
width
height
visible
focused
parent_window_id
surface_type
uses_cheesebridge
```
This table lets HaSe decide whether a window is a normal UI window, a Steam window, a launcher window, a popup, or a game render surface.

The first implementation does not need perfect rootless windows. It needs stable window tracking, correct input mapping, correct cropping, and acceptable behavior for Steam login and launching one simple game.

Audio, Clipboard, and File Sharing

Audio should be handled separately from window management. The first path can use PulseAudio or PipeWire inside the Linux guest and forward audio to a macOS host helper. This does not need to be solved by CheeseBridge.

Clipboard support should be explicit. HaSe should sync text clipboard data between macOS and the Linux guest. File drag and drop can come later.

File sharing should be mounted into the Linux environment. The bottle needs access to selected macOS folders, Steam library paths, downloads, and per-bottle storage. The first Lima-based version can use Lima file sharing. A later native VM backend can use VirtioFS or another shared filesystem.

## Launch Sequence

The launch sequence should be deterministic.
```text
User opens HaSe bottle
        ↓
HaSe starts hidden Linux VM if needed
        ↓
HaSe starts CheeseBridge host
        ↓
HaSe starts window capture and input bridge
        ↓
HaSe starts Linux graphical session
        ↓
HaSe launches Steam or selected app
        ↓
Steam window appears as macOS window
        ↓
User launches game from Steam
        ↓
Game window appears as macOS window

## Stop Sequence
```
The stop sequence should also be controlled.
```text
User closes Steam or bottle
        ↓
HaSe asks Linux apps to exit
        ↓
HaSe waits for Steam and game processes
        ↓
HaSe shuts down CheeseBridge connections
        ↓
HaSe stops capture and input bridge
        ↓
HaSe suspends or shuts down the Linux VM
```
The first goal is not to make the VM disappear technically. The first goal is to make the VM disappear from the user experience.

## Phase 0: Architecture Notes

Goal:

Write the design down, define the rough protocol, and agree on the scope before touching real Vulkan forwarding.

Success condition:

The project has a clear direction and the first implementation target is understood.

## Phase 1: Fake Bridge Demo

Goal:

Create a guest demo program that sends fake Vulkan-like messages to a macOS host process.

Example flow:

```text
Guest sends CREATE_INSTANCE
Host replies OK

Guest sends CREATE_BUFFER
Host replies BUFFER_ID=1

Guest sends QUEUE_SUBMIT
Host logs the submit and replies OK
```

Success condition:

Guest and host can communicate reliably.

## Phase 2: Vulkan ICD Stub

Goal:

Create a minimal Linux Vulkan ICD stub.

The ICD should load through the Vulkan loader, log function calls, respond to basic instance and device queries, connect to the CheeseBridge host, and return fake device information.

Success condition:

vulkaninfo can detect the CheeseBridge ICD or fail with useful logs.

## Phase 3: Minimal Vulkan Forwarding

Goal:

Forward a tiny Vulkan subset to the macOS host.

Target workloads:

```text
Clear screen
Draw triangle
Basic vkcube-style test
```

Success condition:

A simple rendered frame appears on macOS.

## Phase 4: MoltenVK Host Backend

Goal:

Execute reconstructed Vulkan work through MoltenVK on macOS.

Success condition:

The host can create buffers, images, command buffers, and present frames through the MoltenVK path.

## Phase 5: DXVK Smoke Test

Goal:

Run a very small DirectX application through Wine and DXVK inside the guest.(The linux enviroment)

Success condition:

The DirectX application creates a Vulkan device through CheeseBridge and reaches the host renderer.

## Phase 6: First Game Test

Goal:

Run a lightweight Windows game.

Good early targets:

```text
Small indie game
Old DirectX 9 game
Simple DirectX 11 Unity game
Simple demo application with predictable rendering
```

Targets to avoid at this stage:

```text
Anti-cheat games
Heavy DirectX 12 games
Large launchers
AAA games
Games with complex DRM or online services
```

## Protocol Draft

Protocol version: 0.1

## Handshake

Guest sends:

```yaml
type: HELLO
protocol_version: 0.1
guest_name: CheeseBridgeGuest
features_requested:
  - vulkan_basic
  - shared_memory_optional
```

Host replies:

```yaml
type: HELLO_REPLY
protocol_version: 0.1
status: OK
host_name: CheeseBridgeHost
features_supported:
  - vulkan_basic
  - moltenvk_backend_placeholder
```

## Capability Query

Guest sends:

```yaml
type: CAPABILITY_QUERY
```

Host replies:

```yaml
type: CAPABILITY_REPLY
api: Vulkan
backend: MoltenVK
max_buffers: placeholder
max_images: placeholder
supports_present: false
```

## Create Instance

Guest sends:

```yaml
type: CREATE_INSTANCE
application_name: test
api_version: 1.2
```

Host replies:

```yaml
type: CREATE_INSTANCE_REPLY
status: OK
instance_id: 1
```

## Enumerate Physical Devices

Guest sends:

```yaml
type: ENUMERATE_PHYSICAL_DEVICES
instance_id: 1
```

Host replies:

```yaml
type: PHYSICAL_DEVICES_REPLY
devices:
  - device_id: 1
    name: Apple GPU via CheeseBridge
    type: integrated_gpu
```

## Create Device

Guest sends:

```yaml
type: CREATE_DEVICE
physical_device_id: 1
requested_queues:
  - graphics
  - present
```

Host replies:

```yaml
type: CREATE_DEVICE_REPLY
status: OK
device_id: 1
```

## Create Buffer

Guest sends:

```yaml
type: CREATE_BUFFER
device_id: 1
size: 1048576
usage: vertex_buffer
```

Host replies:

```yaml
type: CREATE_BUFFER_REPLY
status: OK
buffer_id: 1
```

## Queue Submit

Guest sends:

```yaml
type: QUEUE_SUBMIT
device_id: 1
queue: graphics
commands:
  - placeholder_command_buffer_id: 1
```

Host replies:

```yaml
type: QUEUE_SUBMIT_REPLY
status: OK
fence_id: 1
```

## Present

Guest sends:

```yaml
type: PRESENT
swapchain_id: 1
image_id: 1
```

Host replies:

```yaml
type: PRESENT_REPLY
status: OK
```

## Non-Goals

CheeseBridge should not start with full DXVK support. (This is for future possible contributors)

It should not start with AAA games, heavy DirectX 12 workloads, anti-cheat titles, launchers, or custom Metal translation.

It should not replace the current MacNCheese backend until the bridge proves basic correctness.

It should not bundle proprietary Apple components.

It should not be presented as production-ready.

## Best First Prototype

The first useful prototype should prove only this:

```text
Linux guest or guest-like demo app
        ↓
Sends fake Vulkan messages
        ↓
macOS CheeseBridge host receives them
        ↓
Host logs them
        ↓
Host replies correctly
```

After that, the project can move to:

```text
Vulkan ICD stub
        ↓ (I love these arrows)
vulkaninfo detection
        ↓
Fake device enumeration
        ↓
Basic command forwarding
        ↓
Simple rendered frame
```

## MacNCheese Backend Modes

Long term, MacNCheese could support more than one compatibility mode.

Native MacNCheese mode:

```text
Wine 11.0
D3DMetal, DXMT, or macOS Vulkan paths
macOS-native execution where possible
```

HaSe mode:

```text
Linux guest
FEX
Wine
DXVK / VKD3D-Proton
CheeseBridge
MoltenVK / Metal
```

HaSe is a research path for future compatibility and post-Rosetta planning. It is not the main backend today.

## Summary

CheeseBridge is the graphics bridge required for HaSe to become useful.

Its job is to act like a virtual Vulkan GPU path:

```text
Guest Vulkan ICD
        ↓
Command serialization
        ↓
VM transport
        ↓
Host renderer
        ↓
MoltenVK or Metal
```

The correct development path is incremental:

```text
Fake bridge demo
Vulkan ICD stub
vulkaninfo detection
Simple triangle
MoltenVK host backend
DXVK smoke test
Small game
Performance work
```

The goal is not to build the full system at once.

The goal is to prove one layer at a time.



ALSO prs or first changes are WELCOME . Thanks.
