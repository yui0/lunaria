/* MPL-2.0. Placeholder image for one library of the ART runtime.
 *
 * A zygote child maps libart.so, libartbase.so, libdexfile.so and
 * libnativeloader.so before app code runs, and process-protection SDKs read
 * /proc/self/maps for them, then parse the ELF image at the address found.
 * Lunaria has no ART, but the maps of the process it imitates must still
 * show them, backed by a readable ELF.  Built once per library with
 * -DART_IMAGE_SYMBOL=<unique exported name>. */
__attribute__((visibility("default")))
int ART_IMAGE_SYMBOL(void)
{
    return 0;
}
