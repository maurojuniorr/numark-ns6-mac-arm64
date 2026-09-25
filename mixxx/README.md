# Numark NS6 mapping for Mixxx

Copy `Numark NS6.midi.xml` and `Numark-NS6-scripts.js` to Mixxx's
`controllers` directory, then restart Mixxx and select **Numark NS6**.

On sandboxed macOS builds, the directory is:

```text
~/Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx/controllers/
```

The mapping uses the `Numark NS6` CoreMIDI input and output supplied by this
driver. Its startup function enables the NS6 LED mode and initializes the
visual feedback.
