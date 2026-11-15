# Diffractor Faces

This document owns face detection: what the detector finds in a picture, the input it reads, and the
space and order it reports faces in. Decoding belongs to [file I/O](file-io.md), and the detector's
provenance and build to [third-party](third-party.md#face-detection).

## What detection finds

Detection finds the faces in one decoded picture. Each face is a box, five landmarks - both eye
centres, the nose tip and both mouth corners - and the detector's confidence as a percentage. It
identifies nobody: no face is compared with another, and nothing about a face is kept.

No view, command or background pass runs detection, and nothing it finds is stored or shown. Its
tests are its only callers.

## The input

The network reads one 8-bit luminance plane, at most 640 pixels on its long edge; a larger picture
is reduced by area averaging first. Detection takes time in proportion to the plane's pixels, and
the smallest face it can find grows as the plane shrinks.

- A packed surface's luminance is weighed from its colour at full range. An NV12 surface's is its Y
  plane, expanded to full range when the decode is limited range. Packed and NV12 decodes of one
  picture reach the detector as the same plane, to within rounding.
- The network was trained on colour, but its first layer weighs each pixel's colours and nothing
  else, so a grey pixel reads like any other colour. What luminance finds agrees with what
  libfacedetection's own entry point finds in colour.
- Any other surface format, P010 among them, finds no faces.

## Upright faces

Faces are found and reported in the space the picture is shown in. Most formats decode in the file's
own orientation, so a phone's portrait photograph arrives on its side, and the network does not find
a face lying on its side. The plane is turned upright once it is reduced, as the picture is turned
for display, which also leaves no box to map back afterwards.

A face set's extent is the dimensions of the surface detection ran on, turned upright. A bounded
decode makes these smaller than the file's own, so a box is placed by scaling from the extent, never
from the file's dimensions.

## What is reported

- A face below 70% confidence, or narrower or shorter than 3% of the picture's short edge, is not
  reported. Both floors and the plane's long edge are one tuning, in
  [model_faces.h](../src/model_faces.h).
- Of two detections overlapping by more than 45% of their union, only the more confident is reported.
- Boxes and landmarks keep sub-pixel precision: measured on the plane, then scaled to the extent. A
  box can reach past the picture's edge where the face does.
- Faces are listed left to right, then top to bottom, then larger first, compared in whole pixels, so
  one picture read again at another size lists its faces in the same order. The detector's own order
  follows its scores, which move.

## The detector

The network is libfacedetection's YuNet, its weights compiled in, run through libfacedetection's
Highway kernels at the instruction-set baseline Diffractor builds for. A detector holds the scratch
buffers its calls reuse, so one serves one thread at a time; the weights every detector reads are
loaded once, on the first detection, and never change. Detection reads only the surface it is handed:
it opens no file, touches no database and needs no window.

## Where this lives

| Concern | Source |
| --- | --- |
| Faces, the tuning, the luminance plane, turning it upright, and the stable order | [model_faces.h](../src/model_faces.h), [model_faces.cpp](../src/model_faces.cpp) |
| The network, the plane packed as it reads it, and the selection of what it finds | [model_faces_detect.cpp](../src/model_faces_detect.cpp) |
| The library's build | [facedetection.cmake](../cmake/vendored/facedetection.cmake), [third-party](third-party.md#face-detection) |
| Tests, over libfacedetection's own sample photographs | [test_faces.cpp](../src/test_faces.cpp) |
