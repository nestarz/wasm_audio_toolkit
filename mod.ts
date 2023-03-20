import ModuleFactory from "./dist/main.js";

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
      cleanSet.push(async () => handler._free(ptr));
      return [ptr, size];
    },
    () => cleanSet.forEach((fn) => fn().catch(console.error)),
  ];
};

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
  const data = await fetch(new URL("./assets/pcm1608m.aac", import.meta.url))
    .then((r) => r.arrayBuffer())
    .then((v) => new Uint8Array(v));
  await run(data, "aac", "ogg", "libopus").then(
    (res) => console.log(res) ?? Deno.writeFile("./dist/out.ogg", res)
  );
  await run(data, "aac", "adts", "libfdk_aac").then((res) =>
    Deno.writeFile("./dist/out.aac", res)
  );
  await run(data, "aac", "mp2", "mp2").then((res) =>
    Deno.writeFile("./dist/out.mp2", res)
  );
  await run(data, "aac", "webm", "libopus").then((res) =>
    Deno.writeFile("./dist/out.webm", res)
  );
}
