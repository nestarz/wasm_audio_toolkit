import ModuleFactory from "./dist/main.js";
import { createDecoder } from "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/src/minimp3-wasm.ts";
import { addWaveHeader } from "https://deno.land/x/wave_header/mod.ts";

const wasmPath =
  "https://raw.githubusercontent.com/bashi/minimp3-wasm/master/dist/decoder.opt.wasm";

const createMalloc = (handler) => {
  const cleanSet = [];
  return [
    (arrOrString) => {
      if (!arrOrString) return [null, 0];
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
  console.log(size);
  const structPtr = handler._probe(avArrayPtr, size, sourceContainerPtr);
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 18);
  const res = {
    outPtr: view.getUint32(0, true),
    size: view.getUint32(4, true),
    sampleRate: view.getUint16(8, true),
    bitDepth: view.getUint16(10, true),
    numChannels: view.getUint16(12, true),
    duration: view.getUint16(14, true),
    frameSize: view.getUint16(16, true),
  };
  free();
  return res;
};

export const transcode = async (
  avArray: Uint8Array,
  sourceContainer: string,
  destContainer: string,
  destCodec: string,
  destContainerOptions?: Record<string, string>,
  initialMemory: number = 128 * 1024 * 1024
) => {
  const destContainerOptionsString = destContainerOptions
    ? Object.entries(destContainerOptions)
        .map((v) => v.join(":"))
        .join(",")
    : null;
  const handler = await ModuleFactory({ INITIAL_MEMORY: initialMemory });
  const [malloc, free] = createMalloc(handler);
  const [avArrayPtr, size] = malloc(avArray);
  const [sourceContainerPtr] = malloc(sourceContainer);
  const [destContainerPtr] = malloc(destContainer);
  const [destCodecPtr] = malloc(destCodec);
  const [destContainerOptionsPtr] = malloc(destContainerOptionsString);
  const structPtr = handler._transcode(
    avArrayPtr,
    size,
    sourceContainerPtr,
    destContainerPtr,
    destCodecPtr,
    destContainerOptionsPtr
  );
  const view = new DataView(handler.HEAPU8.buffer, structPtr, 10);
  const res = {
    outPtr: view.getUint32(0, true),
    size: view.getUint32(4, true),
    sample_rate: view.getUint16(8, true),
  };
  const data = new Uint8Array(
    new Uint8Array(handler.HEAPU8.buffer, res.outPtr, res.size)
  );
  console.log(data);
  free();
  return { data, ...res };
};

// const data = await fetch(new URL("output.mp4", import.meta.url))
//   .then((r) => r.arrayBuffer())
//   .then((buffer) => new Uint8Array(buffer))
//   .then(async (data) => {
//     const { sampleRate, numChannels } = await probe(data, "mov");
//   });

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
      const { sampleRate, numChannels } = await probe(data, "wav");
      console.log(sampleRate, numChannels);
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
