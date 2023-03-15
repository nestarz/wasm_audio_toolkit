import ModuleFactory from "./dist/main.js";

export const profiles = { AAC: 0, MP2: 1 };

export const run = async (inputArray: any, profileId: number) => {
  const handler = await ModuleFactory({
    INITIAL_MEMORY: 2048 * 1024 * 1024, // 2048 MB in bytes
  });

  const size = inputArray.length * 2;
  const ptr = handler._malloc(size);
  handler.HEAPU8.set(inputArray, ptr);

  const outSize = handler._modify_array(ptr, size, profileId);

  const modifiedArray = new Uint8Array(handler.HEAPU8.buffer, ptr, outSize);
  console.log("Modified Array: ", modifiedArray);
  handler._free(ptr);
  return modifiedArray;
};

if (import.meta.main) {
  const data = await fetch(new URL("./assets/pcm1608m.wav", import.meta.url))
    .then((r) => r.arrayBuffer())
    .then((v) => new Uint8Array(v));
  await run(data, profiles.AAC).then((res) =>
    Deno.writeFile("./dist/out.aac", res)
  );
  await run(data, profiles.MP2).then((res) =>
    Deno.writeFile("./dist/out.mp2", res)
  );
}
