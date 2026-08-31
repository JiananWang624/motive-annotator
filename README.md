# Motive Annotation Monitor v3.1

## Final design

Motive remains the only playback and frame-stepping interface.

This tool adds only:

- NatNet Frame ID listening;
- configurable global annotation hotkeys;
- an always-on-top status window;
- one annotation CSV per Take;
- duplicate protection;
- sequence warnings;
- undo;
- explicit Correction Mode.

This keeps the annotation layer small and auditable.

## Install

Put these files in the official OptiTrack NatNet Python sample folder:

    motive_annotator.py
    annotation_config.json

The same folder must contain the official SDK file:

    NatNetClient.py

Install:

    py -m pip install pynput

`tkinter` normally comes with standard Windows Python.

## Motive

Open the `.tak` Take and enable NatNet / Broadcast Frame Data.

For Motive and Python on the same Windows PC, start with:

    Local Interface: Loopback / 127.0.0.1
    Transmission: Multicast

## Run

    py motive_annotator.py --take P01_trial01

Output:

    annotations\P01_trial01.annotations.csv

Each Take has its own CSV. Reopening the same Take automatically restores the
existing annotation state in the monitor.

## Default event hotkeys

    Ctrl + Alt + Shift + A   Approach
    Ctrl + Alt + Shift + M   Interaction
    Ctrl + Alt + Shift + C   Contact
    Ctrl + Alt + Shift + R   Withdrawal

Actions:

    Ctrl + Alt + Shift + E   Enter Correction Mode
    Ctrl + Alt + Shift + U   Undo last change made in this run
    Ctrl + Alt + Shift + N   Next bite
    Ctrl + Alt + Shift + P   Previous bite
    Ctrl + Alt + Shift + Q   Quit

All shortcuts can be changed in `annotation_config.json`.

Before formal annotation, compare these with your own Motive hotkey settings.

## Monitor

The always-on-top window shows, in real time:

    P05_trial01                       Bite 1

    Frame
    18342                            STABLE 0.8s
                                     NORMAL

    Approach       17620 ✓
    Interaction    18210 ✓
    Contact        18342 ✓
    Withdrawal     -----

    Sequence: OK

    SAVED: B1 Contact @ frame 18342

    Recent
    B1 Approach     17620
    B1 Interaction  18210
    B1 Contact      18342

This is the live audit interface. You do not need Excel open during annotation.

## Normal annotation workflow

    Play in Motive
        ↓
    event is close
        ↓
    Pause
        ↓
    frame-step to the exact frame
        ↓
    compare Motive Frame Counter with monitor Frame
        ↓
    wait until monitor says STABLE
        ↓
    press event hotkey
        ↓
    confirm saved Frame ID appears beside that event

The default stable-frame gate is 300 ms.

While Motive frames are changing, the annotator refuses to save.

## Correcting an older label

Normal mode NEVER silently overwrites an existing event.

If you discover that Bite 1 Contact was incorrectly labeled at frame 18342:

1. Pause/step Motive to the correct frame, e.g. 18348.
2. Wait for `STABLE`.
3. Press:

       Ctrl + Alt + Shift + E

4. The monitor shows `CORRECTION`.
5. Within 10 seconds, press the Contact hotkey:

       Ctrl + Alt + Shift + C

The CSV changes deliberately from:

    Contact 18342

to:

    Contact 18348

The monitor reports:

    CORRECTED: B1 Contact 18342 -> 18348

You can Undo that correction during the same program run.

Correction Mode times out automatically and is canceled when changing bites.

## Undo

Undo reverses the last ADD or CORRECTION made during the current program run.

It does not blindly delete old annotations loaded from an earlier session.

## Multiple bites

Use Next/Previous Bite.

If you leave a partially labeled bite, the monitor warns which configured events
are missing, but it does not block you.

## Sequence check

Configured event order is the expected temporal order.

Default:

    Approach < Interaction < Contact < Withdrawal

If stored frames violate this order, the monitor immediately displays a warning.

This is a warning only; it never silently edits your data.

## Output CSV

Example:

    take,bite_id,event,motive_frame,natnet_timestamp_s,annotated_at
    P01_trial01,1,approach_start,17620,12.341667000,...
    P01_trial01,1,interaction_entry,18210,17.258333000,...
    P01_trial01,1,contact,18342,18.358333000,...
    P01_trial01,1,withdrawal_start,18410,18.925000000,...

Use `motive_frame` as the authoritative key.

Later:

    annotation motive_frame
            ↓
    Motive exported CSV Frame ID
            ↓
    fork / wrist / elbow / shoulder data

`natnet_timestamp_s` is retained only as diagnostic metadata.

## First validation before bulk annotation

Before processing all participants:

1. Pause one Take at an obvious Motive frame such as 1000.
2. Frame-step once.
3. Confirm the monitor also shows Frame 1000.
4. Wait for STABLE.
5. Create a test annotation.
6. Confirm CSV stores exactly frame 1000.
7. Repeat near the END of the Take.

Only start bulk annotation if both checks match exactly.

## Excel

Do not leave a Take's annotation CSV open in Excel while labeling. Excel may lock
the file on Windows.

The program uses atomic CSV writes. If the file is locked, it reports the failure
instead of pretending the annotation was saved.

## Unicast fallback

If multicast does not work, configure Motive for Unicast and run:

    py motive_annotator.py --take P01_trial01 --unicast

## Intentionally not included

This tool does not:

- parse `.tak` directly;
- replace Motive's timeline;
- make another video/player UI;
- modify marker data;
- infer semantic feeding events;
- silently overwrite annotations.

Those are deliberate scope choices to keep the tool simple and reliable.


# 运行命令 
python motive_annotator.py --take <实际Take名称>