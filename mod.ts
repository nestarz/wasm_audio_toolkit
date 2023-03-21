import ModuleFactory from "./dist/main.js";
import { createDecoder } from "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/src/minimp3-wasm.ts";
import { addWaveHeader } from "https://deno.land/x/wave_header/mod.ts";

const wasmPath =
  "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/dist/decoder.opt.wasm";

export const profiles = { AAC: 0, MP2: 1, WEBM_OPUS: 2, MP3: 3, OGG_OPUS: 4 };

export const run = async (
  avArray: Uint8Array,
  sourceContainer: string,
  destContainer: string,
  destCodec: string,
  initialMemory: number = 128 * 1024 * 1024
) => {
  const handler = await ModuleFactory({ INITIAL_MEMORY: initialMemory });
  const [malloc, free] = createMalloc(handler);
  const [avArrayPtr, size] = malloc(avArray);
  const [sourceContainerPtr] = malloc(sourceContainer);
  const [destContainerPtr] = malloc(destContainer);
  const [destCodecPtr] = malloc(destCodec);
  const modifiedArray = new Uint8Array(
    new Uint8Array(
      handler.HEAPU8.buffer,
      avArrayPtr,
      handler._transcode(
        avArrayPtr,
        size * 2,
        sourceContainerPtr,
        destContainerPtr,
        destCodecPtr
      )
    )
  );
  free();
  return modifiedArray;
};

if (import.meta.main) {
  const data = await fetch(
    new URL(
      "radio/assets/Extreme-Trax-Final-Fantasy-CaRaVEL-EDIT.wav",
      import.meta.url
    )
  )
    .then((r) => r.arrayBuffer())
    .then((v) => new Uint8Array(v));
  await run(data, "wav", "adts", "libfdk_aac").then((res) =>
    Deno.writeFile("./dist/out.aac", res)
  );
  await run(data, "wav", "ogg", "libopus").then((res) =>
    Deno.writeFile("./dist/out.ogg", res)
  );
  await run(data, "wav", "mp2", "mp2").then((res) =>
    Deno.writeFile("./dist/out.mp2", res)
  );
  await run(data, "wav", "webm", "libopus").then((res) =>
    Deno.writeFile("./dist/out.webm", res)
  );
}
