#pragma once
// Permanent fix: center world-map location-name labels on their measured VWF
// width (the engine's own width function mis-counts VWF-narrowed glyphs and
// zero-advance spaces). Always on; no config. See label_center.cpp.
void label_center_install();
