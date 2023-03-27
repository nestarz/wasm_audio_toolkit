import { addWaveHeader } from "https://deno.land/x/wave_header@0.0.1/mod.ts";
import { transcode, probe } from "./mod.ts";

if (import.meta.main) {
  const data = await fetch(
    new URL(
      "radio/assets/Extreme-Trax-Final-Fantasy-CaRaVEL-EDIT.wav",
      import.meta.url
    )
  )
    .then((r) => r.arrayBuffer())
    .then((buffer) => new Uint8Array(buffer))
    .then(async (data) => {
      const { sampleRate, numChannels, duration } = await probe(data, "wav");
      console.log(duration, sampleRate, numChannels);
      const chunkSize = 1920000;
      const start = chunkSize * 30;
      return new Uint8Array(
        addWaveHeader(
          new Int16Array(data.slice(start, start + chunkSize).buffer),
          numChannels,
          sampleRate,
          16
        ).buffer
      );
    });
  await transcode(data, "wav", "adts", "libfdk_aac").then(({ data }) =>
    Deno.writeFile("./dist/out.aac", data)
  );
  await transcode(data, "wav", "mp4", "libfdk_aac", {
    movflags: "+dash+delay_moov+skip_sidx+skip_trailer",
  }).then(({ data }) => Deno.writeFile("./dist/out.mp4", data));
}
