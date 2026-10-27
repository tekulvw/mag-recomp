#include <stdio.h>
#ifdef MAG_SAMPLE
int mag_sample_extension(void);
int main(void) { puts("extension"); return mag_sample_extension() == 42 ? 0 : 1; }
#else
int main(void) { puts("no extension"); return 0; }
#endif
