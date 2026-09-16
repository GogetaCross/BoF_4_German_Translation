// title_jp.h — Japanese title-card draw list on the Western build.
#pragma once

// Detours draw_title_logo / draw_copyright and re-emits the JP build's own
// primitives. Safe no-op unless g_cfg.jp_title is on AND the byte guards match.
void title_jp_install();
void title_jp_shutdown();
