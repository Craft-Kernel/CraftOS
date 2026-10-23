# the pictures, fonts and pages built right into the kernel. each file is
# copied in byte for byte, with a label at its start and another at its
# end, and sysfiles.c turns them into read-only files on the ram disk. the
# pictures are .cimg files, made from pngs by tools/img2cimg.py.

.section .rodata

.macro ASSET name, file
.global asset_\name
.global asset_\name\()_end
.balign 4
asset_\name:
    .incbin "\file"
asset_\name\()_end:
.endm

ASSET wallpaper, "assets/wallpaper.cimg"
ASSET craft_16, "assets/craft-16.cimg"
ASSET craft_32, "assets/craft-32.cimg"
ASSET craft_48, "assets/craft-48.cimg"

# the status icons: info, question, warning and fail, each at 16, 32, 40 and 48 px.
ASSET info_16, "assets/info-16.cimg"
ASSET info_32, "assets/info-32.cimg"
ASSET info_40, "assets/info-40.cimg"
ASSET info_48, "assets/info-48.cimg"
ASSET question_16, "assets/question-16.cimg"
ASSET question_32, "assets/question-32.cimg"
ASSET question_40, "assets/question-40.cimg"
ASSET question_48, "assets/question-48.cimg"
ASSET warning_16, "assets/warning-16.cimg"
ASSET warning_32, "assets/warning-32.cimg"
ASSET warning_40, "assets/warning-40.cimg"
ASSET warning_48, "assets/warning-48.cimg"
ASSET fail_16, "assets/fail-16.cimg"
ASSET fail_32, "assets/fail-32.cimg"
ASSET fail_40, "assets/fail-40.cimg"
ASSET fail_48, "assets/fail-48.cimg"
