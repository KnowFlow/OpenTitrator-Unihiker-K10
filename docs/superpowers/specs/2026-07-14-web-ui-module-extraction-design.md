# Web UI Module Extraction Design

## Goal

Move browser-page rendering out of `ph_titrator.ino` while leaving routes,
authentication, device control, JSON telemetry, and experiment state unchanged.

## Boundary

`web_ui.h/.cpp` will expose `String renderWebPage(const WebUiModel&)`. The
model is a value snapshot of every server-rendered item: network labels,
sensor/current-run facts, calibration values, pump settings, method settings,
and display labels. The sketch remains responsible for collecting those values
and calls the renderer from its existing `htmlPage()` wrapper.

The browser JavaScript is copied byte-for-byte in the first migration. No
JavaScript behavior, endpoints, session handling, IndexedDB behavior, or page
layout changes are permitted by this extraction.

## Migration and validation

1. Define the model and renderer interface.
2. Move static HTML/JavaScript and model substitutions to the module.
3. Replace the sketch implementation with a model-construction wrapper.
4. Add a static boundary test ensuring `htmlPage()` delegates to the renderer,
   while route handlers remain in the sketch.
5. Run existing browser/static tests and PlatformIO build.
