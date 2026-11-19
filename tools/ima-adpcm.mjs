// IMA ADPCM's standard 89 step sizes. The pack stores the first sample and
// index explicitly, followed by low-nibble-first codes for the remaining samples.
const steps = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
  1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
  11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const advance = [-1, -1, -1, -1, 2, 4, 6, 8];
const clamp16 = (sample) => Math.max(-32768, Math.min(32767, sample));

// Select the nearest of the eight possible magnitudes. This also produces
// the reconstructed PCM used by the exporter to quantify codec error.
export function encodeVoice(pcm) {
  const frames = pcm.length / 2;
  if (!frames || frames > 30 * 24000) throw new Error("Invalid voice duration.");
  const codes = Buffer.alloc(frames >> 1);
  const first = pcm.readInt16LE(0);
  const firstChange = frames > 1 ? Math.abs(pcm.readInt16LE(2) - first) : 0;
  const seed = steps.findIndex((step) => step >= firstChange);
  const startIndex = seed < 0 ? 88 : seed;
  let predictor = first;
  let index = startIndex;
  let signal = first * first;
  let error = 0;
  const reconstructed = Buffer.alloc(pcm.length);
  reconstructed.writeInt16LE(first, 0);
  for (let sample = 1; sample < frames; sample++) {
    const target = pcm.readInt16LE(sample * 2);
    const step = steps[index];
    const sign = target < predictor ? -1 : 1;
    let bestError = Infinity;
    let closest = predictor;
    let code = 0;
    for (let magnitude = 0; magnitude < 8; magnitude++) {
      const diff = (step >> 3) + (magnitude & 4 ? step : 0) + (magnitude & 2 ? step >> 1 : 0) + (magnitude & 1 ? step >> 2 : 0);
      const estimate = clamp16(predictor + sign * diff);
      const distance = Math.abs(estimate - target);
      if (distance >= bestError) continue;
      closest = estimate;
      bestError = distance;
      code = magnitude;
    }
    index = Math.max(0, Math.min(88, index + advance[code]));
    predictor = closest;
    if (sign < 0) code |= 8;
    const nibble = sample - 1;
    codes[nibble >> 1] |= code << ((nibble & 1) * 4);
    signal += target * target;
    error += bestError * bestError;
    reconstructed.writeInt16LE(closest, sample * 2);
  }
  return { frames, predictor: first, index: startIndex, codes, reconstructed, signal, error };
}

export function wav(pcm) {
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(pcm.length + 36, 4);
  header.write("WAVEfmt ", 8);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(24000, 24);
  header.writeUInt32LE(48000, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}
