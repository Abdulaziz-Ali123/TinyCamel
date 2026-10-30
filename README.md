# Tiny Camel 🐫

**End-to-end TinyML driving on an ESP32-S3: a camera, a quantized CNN, and a toy car, all running on a microcontroller.**

Tiny Camel replicates and extends [DeepPicarMicro](https://arxiv.org/pdf/2208.11212) on the Seeed Studio ESP32-S3. A MobileNetV2 classifier reads camera frames and decides whether the car should go left, forward, or right, with no laptop or cloud involved. We built two vehicle platforms and studied what happens to the accuracy-latency trade-off when you move to a more capable MCU.

📄 **[Read the full report (PDF)](TinyCamelReport.pdf)**

**Team:** Harry Wang, Abdulaziz Ali, Isaac Mohabbat (University of Kansas)

---

## Key Results

| Metric | Result |
|---|---|
| Validation accuracy (float model) | ~87% |
| Test accuracy (float model) | ~86% |
| Accuracy after full Int8 quantization | ~67% |
| On-device inference latency | ~800 ms per frame |
| Target control deadline | 33-50 ms (20-30 FPS) |
| Initial end-to-end loop time | ~1.5 s (blocking `delay()` calls plus slow inference) |

**Bottom line:** the pipeline works end to end, but inference was roughly 16-24x slower than our real-time target, so the car could not drive the track reliably. Most of what we learned came from figuring out why.

---

## How It Works

```
Camera → Crop / Grayscale / Resize → Blue-tape mask → Quantized MobileNetV2 → Action (L / F / R) → Motor driver
```

1. **Frame capture** from an OV2640/OV5640 camera
2. **Preprocessing:** grayscale conversion, adaptive thresholding, HSV filtering to isolate the blue tape, morphological noise removal, resize to 96×96
3. **Inference:** Int8-quantized MobileNetV2 via TensorFlow Lite Micro
4. **Decision logic:** an action fires only if its softmax probability exceeds 0.80
5. **Motor control:** binary on/off commands to the motor driver

### Model

- Input: 96×96 grayscale image
- Backbone: MobileNetV2 (reduced width multiplier)
- Head: three dense layers with batch normalization and dropout
- Output: 3-way softmax (left-forward, forward, right-forward)
- Trained in TensorFlow with augmentation (horizontal flips, brightness shifts, small rotations), then converted with post-training Int8 quantization

---

## Hardware

We built two vehicles to compare control paradigms.

| | Traditional RC car | Differential-drive servo car |
|---|---|---|
| Steering | Front-wheel steering, rear-wheel drive | Two modified continuous-rotation servos |
| Motor driver | L298N H-bridge | Arduino Uno (voltage conversion and servo parsing) |
| Power | 7.4V LiPo | - |
| Strength | Conventional vehicle dynamics | Finer motion control |
| Weakness | Speed-dependent turning radius, binary fast/stalled motors | Extra electronics added latency and reliability issues |

Both use the Seeed Studio ESP32-S3 (dual-core Xtensa @ 240 MHz, 512KB SRAM, optional PSRAM).

---

## Track and Data

- **Track:** a single lane of blue tape on the floor. This keeps it easy to reproduce at home, but it means that once the car leaves the tape there are no boundary cues to recover from.
- **Data collection:** imitation learning. We drove the car around the track by hand while recording synchronized camera frames and steering commands.
- **Known data issues:** inconsistent human driving led to class imbalance, with far fewer left-turn examples than forward or right.

---

## What We Learned

- **Faster MCU ≠ faster inference.** Inference took ~800 ms on an ESP32-S3, far above what the clock speed suggests. We suspect immature TFLM optimization for the S3's vector instructions, poor mapping of MobileNetV2's depthwise convolutions, memory bandwidth, or thermal throttling. We did not isolate which.
- **Blocking code breaks real-time control.** Arduino `delay()` calls added over half a second of dead time per loop. Non-blocking FreeRTOS tasks are the right design.
- **Latency plus speed equals stale commands.** At high speed, the car was well past the scene by the time a command arrived. Slowing down to "stop and think" caused motor stalls and jerky motion, which didn't match the smooth driving the model was trained on.
- **Data quality caps everything.** The model learned the biases of the training data, and the single-lane track meant it never saw recovery examples.
- **Hardware matters to ML.** Speed-dependent steering and coarse motor control made clean training data hard to collect.
- **Quantization has a cost.** Accuracy dropped from ~86% to ~67% after full Int8 quantization.

---

## Future Work

- **Hardware:** higher-torque geared motors, finer steering control, wheel encoders or an IMU for feedback
- **Data:** 5-10× more samples with balanced classes, more robust tape isolation under varying light, simulation pre-training
- **Model:** compare PilotNet and custom small CNNs, mixed-precision quantization, structured pruning
- **Systems:** proper FreeRTOS task scheduling, systematic profiling, automated model deployment tests

Our goal is sub-100 ms inference with reliable autonomous control on the ESP32-S3.

---

## References

1. M. Bojarski et al., "End-to-End Deep Learning for Self-Driving Cars," NVIDIA, 2016.
2. B. C. Jung et al., "[DeepPicar: A Low-Cost Deep Neural Network-Based Autonomous Car](https://arxiv.org/abs/1712.08644)," 2018.
3. S. Alyamkin et al., "[DeepPicarMicro: Applying TinyML to Autonomous Cyber-Physical Systems](https://arxiv.org/abs/2208.11212)," 2022.
