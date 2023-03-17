import ModuleFactory from "./dist/main.js";

export const profiles = { AAC: 0, MP2: 1, WEBM_OPUS: 2, MP3: 3, OGG_OPUS: 4 };

export const run = async (
  inputArray: any,
  sample_rate: number,
  num_channels: number,
  profileId: number
) => {
  const handler = await ModuleFactory({
    INITIAL_MEMORY: 128 * 1024 * 1024, // 2048 MB in bytes
  });
  const size = inputArray.length * 2;
  const ptr = handler._malloc(size);
  handler.HEAPU8.set(inputArray, ptr);
  const modifiedArray = new Uint8Array(
    new Uint8Array(
      handler.HEAPU8.buffer,
      ptr,
      handler._encode_mux(ptr, size, sample_rate, num_channels, profileId)
    )
  );
  handler._free(ptr);
  return modifiedArray;
};

if (import.meta.main) {
  const data = await fetch(new URL("./assets/pcm1608m.wav", import.meta.url))
    .then((r) => r.arrayBuffer())
    .then((v) => new Uint8Array(v));
  await run(data, 8000, 1, profiles.OGG_OPUS).then((res) =>
    Deno.writeFile("./dist/out.ogg", res)
  );
  await run(data, 8000, 1, profiles.MP2).then((res) =>
    Deno.writeFile("./dist/out.mp2", res)
  );
  await run(data, 8000, 1, profiles.WEBM_OPUS).then((res) =>
    Deno.writeFile("./dist/out.webm", res)
  );
  await run(data, 8000, 1, profiles.AAC).then((res) =>
    Deno.writeFile("./dist/out.aac", res)
  );
}
