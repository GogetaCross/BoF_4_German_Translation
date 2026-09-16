// Phase 3 — Hook BOF4's universal "begin render text" entrypoint at
// 0x00527970, swap its single buffer-pointer arg to our own heap buffer
// when a translation is available. Length-unlimited live edits because
// we own the destination buffer; no in-place mutation, no boundary check,
// no page-break cache fixup.
//
// Reads from the live table loaded by text_subst.cpp (shared via
// text_subst.h). On F8 reload, text_subst calls text_resolver_on_reload()
// to flush our heap-buffer cache.
#pragma once

void text_resolver_install();
void text_resolver_on_reload();
