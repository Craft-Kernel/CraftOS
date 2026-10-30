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

# the ui font, dejavu sans at 12 px, regular and bold (see tools/ and the readme).
ASSET sans_12, "assets/sans-12.cfnt"
ASSET sans_bold_12, "assets/sans-bold-12.cfnt"

# solitaire's cards: the 52 faces in one sheet, the backs and empty slots in another.
ASSET sol_cards, "assets/sol-cards.cimg"
ASSET sol_extras, "assets/sol-extras.cimg"

# the app icons, from the craftos app icons design: files, term, tasks, settings,
# notes, photos, mines, trash and solitaire at 16, 28, 32 and 48 px,
# plus the white start-button cube at 16 and 22 px.
ASSET app_files_16, "assets/app-files-16.cimg"
ASSET app_files_28, "assets/app-files-28.cimg"
ASSET app_files_32, "assets/app-files-32.cimg"
ASSET app_files_48, "assets/app-files-48.cimg"
ASSET app_term_16, "assets/app-term-16.cimg"
ASSET app_term_28, "assets/app-term-28.cimg"
ASSET app_term_32, "assets/app-term-32.cimg"
ASSET app_term_48, "assets/app-term-48.cimg"
ASSET app_tasks_16, "assets/app-tasks-16.cimg"
ASSET app_tasks_28, "assets/app-tasks-28.cimg"
ASSET app_tasks_32, "assets/app-tasks-32.cimg"
ASSET app_tasks_48, "assets/app-tasks-48.cimg"
ASSET app_settings_16, "assets/app-settings-16.cimg"
ASSET app_settings_28, "assets/app-settings-28.cimg"
ASSET app_settings_32, "assets/app-settings-32.cimg"
ASSET app_settings_48, "assets/app-settings-48.cimg"
ASSET app_notes_16, "assets/app-notes-16.cimg"
ASSET app_notes_28, "assets/app-notes-28.cimg"
ASSET app_notes_32, "assets/app-notes-32.cimg"
ASSET app_notes_48, "assets/app-notes-48.cimg"
ASSET app_photos_16, "assets/app-photos-16.cimg"
ASSET app_photos_28, "assets/app-photos-28.cimg"
ASSET app_photos_32, "assets/app-photos-32.cimg"
ASSET app_photos_48, "assets/app-photos-48.cimg"
ASSET app_mines_16, "assets/app-mines-16.cimg"
ASSET app_mines_28, "assets/app-mines-28.cimg"
ASSET app_mines_32, "assets/app-mines-32.cimg"
ASSET app_mines_48, "assets/app-mines-48.cimg"
ASSET app_trash_16, "assets/app-trash-16.cimg"
ASSET app_trash_28, "assets/app-trash-28.cimg"
ASSET app_trash_32, "assets/app-trash-32.cimg"
ASSET app_trash_48, "assets/app-trash-48.cimg"
ASSET app_solitaire_16, "assets/app-solitaire-16.cimg"
ASSET app_solitaire_28, "assets/app-solitaire-28.cimg"
ASSET app_solitaire_32, "assets/app-solitaire-32.cimg"
ASSET app_solitaire_48, "assets/app-solitaire-48.cimg"
ASSET app_start_16, "assets/app-start-16.cimg"
ASSET app_start_22, "assets/app-start-22.cimg"

