Android 12.0.0_r1 MIME inputs.

android.mime.types and vendor.mime.types: https://android.googlesource.com/platform/frameworks/base/+/android-12.0.0_r1/mime/java-res/ (AOSP Apache-2.0).

debian.mime.types: https://android.googlesource.com/platform/external/mime-support/+/android-12.0.0_r1/mime.types (public domain; LICENSE.mime-support).

Run `python3 scripts/generate-mime-tables.py` to regenerate src/dvm/mime_data.h. The generator implements MimeMap.Builder.addMimeMapping and DefaultMimeMapFactory overlay rules.
