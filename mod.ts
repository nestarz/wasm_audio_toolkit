import ModuleFactory from "./dist/main.js";
import { createDecoder } from "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/src/minimp3-wasm.ts";
import { addWaveHeader } from "https://deno.land/x/wave_header/mod.ts";

const wasmPath =
  "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/dist/decoder.opt.wasm";

const createMalloc = (handler) => {
  const cleanSet = [];
  return [
    (arrOrString) => {
      const inputArray =
        typeof arrOrString === "string"
          ? new TextEncoder().encode(arrOrString + "\0")
          : arrOrString;
      const size = inputArray.length;
      const ptr = handler._malloc(size);
      handler.HEAPU8.set(inputArray, ptr);
      cleanSet.push(() => handler._free(ptr));
      return [ptr, size];
    },
    () => cleanSet.forEach((fn) => fn()),
  ];
};

export const probe = async (avArray: Uint8Array, sourceContainer: string) => {
  const handler = await ModuleFactory({ INITIAL_MEMORY: 128 * 1024 * 1024 });
  const [malloc, free] = createMalloc(handler);
  const [avArrayPtr, size] = malloc(avArray);
  const [sourceContainerPtr] = malloc(sourceContainer);
  const structPtr = handler._probe(avArrayPtr, size * 2, sourceContainerPtr);
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 12);
  const res = {
    size: view.getUint16(0, true),
    sampleRate: view.getUint16(2, true),
    bitDepth: view.getUint16(4, true),
    numChannels: view.getUint16(6, true),
    duration: view.getUint16(8, true),
    frameSize: view.getUint16(10, true),
  };
  console.log(res);
  free();
  return res;
};

export const transcode = async (
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

  const structPtr = handler._transcode(
    avArrayPtr,
    size * 2,
    sourceContainerPtr,
    destContainerPtr,
    destCodecPtr
  );
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 4);
  const res = {
    size: view.getUint16(0, true),
    sample_rate: view.getUint16(2, true),
  };
  const data = new Uint8Array(
    new Uint8Array(handler.HEAPU8.buffer, avArrayPtr, res.size)
  );
  free();
  return { data, ...res };
};

if (import.meta.main) {
  const data = await fetch(
    new URL(
      "radio/assets/Extreme-Trax-Final-Fantasy-CaRaVEL-EDIT.wav",
      import.meta.url
    )
  )
    .then((r) => r.arrayBuffer())
    .then((v) =>
      addWaveHeader(new Uint8Array(v).slice(0, 2 * 1024 * 1024), 2, 48_000, 16)
    );
  // .then(async (arr) => {
  //   const decoder = await createDecoder(arr, wasmPath);
  //   const { pcm, samplingRate, numChannels } = decoder.decode(
  //     decoder.duration
  //   );
  //   return addWaveHeader(
  //     new Uint8Array(pcm.buffer),
  //     numChannels,
  //     samplingRate,
  //     16
  //   );
  // });
  await transcode(data, "wav", "adts", "libfdk_aac").then(({ data }) =>
    Deno.writeFile("./dist/out.aac", data)
  );
  // await transcode(data, "wav", "ogg", "libopus").then(({ data }) =>
  //   Deno.writeFile("./dist/out.ogg", data)
  // );
  // await transcode(data, "wav", "mp2", "mp2").then(({ data }) =>
  //   Deno.writeFile("./dist/out.mp2", data)
  // );
  // await transcode(data, "wav", "webm", "libopus").then(({ data }) =>
  //   Deno.writeFile("./dist/out.webm", data)
  // );
}
