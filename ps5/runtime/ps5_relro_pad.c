/* 2 KB of read-only-after-relocation data (a const array of pointers lands in .data.rel.ro in a PIE), linked in by
 * tools/ps5.py only when the image's RELRO content ends too close to its page boundary for ps5-native-tool to put the
 * PS5 process parameters after it ("no room for PS5 process parameters"). Shifting the end by half a page leaves it
 * room; nothing reads this. From the gta4-ps5 port (ps5/tools/ps5_relro_pad.c, GPL-3.0). */
__attribute__((used, retain, visibility("hidden"))) void *const ps5_relro_pad[256] = {(void *)&ps5_relro_pad};
