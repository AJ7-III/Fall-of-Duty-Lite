import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import { flatMat, makeCanvasTexture } from "../reference/browser/src/rendering/materials/canvas.ts";
import { prim, loft } from "../reference/browser/src/viewmodels/kit.ts";
import { buildSniperViewModel } from "../reference/browser/src/viewmodels/SniperViewModel.ts";

// Native viewmodel units. The right-side deck dips below both the shaft and
// the handle as the bolt travels, then rises again along the fore-end.
const stations = [
  [-.473, .028, -.006, .099, .099],
  [-.461, .032, -.009, .101, .101],
  [-.421, .031, .003, .109, .109],
  [-.373, .029, .014, .108, .108],
  [-.332, .025, .024, .102, .102],
  [-.300, .021, .023, .091, .073],
  [-.276, .021, .012, .092, .063],
  [-.246, .024, -.002, .091, .058],
  [-.222, .027, .006, .092, .057],
  [-.191, .028, .022, .094, .058],
  [-.147, .030, .025, .096, .058],
  [-.126, .030, .026, .097, .067],
  [-.105, .031, .028, .101, .100],
  [ .028, .030, .030, .101, .101],
  [ .101, .027, .034, .097, .097],
  [ .177, .025, .037, .090, .090],
  [ .211, .023, .042, .085, .085],
  [ .222, .021, .045, .081, .081],
];
const cheek = [
  [-.436, .021, .105, .113, .113], [-.428, .024, .103, .122, .122],
  [-.384, .024, .105, .125, .125], [-.352, .020, .102, .116, .116],
  [-.341, .015, .099, .107, .107],
];

function section([z, w, lo, left, right]) {
  const bevel = Math.min(w * .25, (Math.min(left, right) - lo) * .35);
  return [
    new Vector3(-w + bevel, lo, z), new Vector3(w - bevel, lo, z),
    new Vector3(w, lo + bevel, z), new Vector3(w, right - bevel, z),
    new Vector3(w - bevel, right, z), new Vector3(w * .18, left, z),
    new Vector3(-w + bevel, left, z), new Vector3(-w, left - bevel, z),
    new Vector3(-w, lo + bevel, z),
  ];
}

function stockLoft(name, parent, material, outline) {
  const cap = ([z, , lo, left, right]) => Array(9).fill(new Vector3(0, (lo + Math.min(left, right)) / 2, z));
  const pathArray = [cap(outline[0]), ...outline.map(section), cap(outline.at(-1))];
  return prim(
    MeshBuilder.CreateRibbon(name, { pathArray, closePath: true, invertUV: true }, parent.getScene()),
    material, parent, null,
  );
}

function buildStock(parent, rubber) {
  const scene = parent.getScene();
  const texture = makeCanvasTexture(scene, "liteGraphiteStock", 128, (ctx, size) => {
    const image = ctx.createImageData(size, size);
    for (let i = 0; i < size * size; i++) {
      const x = i % size, y = Math.floor(i / size);
      const hash = (Math.imul(x + 37, 133039) ^ Math.imul(y + 71, 28933)) >>> 0;
      const grain = ((hash ^ (hash >>> 11)) % 7) - 3;
      const fiber = .5 * Math.sin(x * .23 + y * .037) + 1.1 * Math.sin(y * .17);
      image.data.set([78 + grain + fiber, 76 + grain + fiber, 77 + grain + fiber, 255], i * 4);
    }
    ctx.putImageData(image, 0, 0);
  });
  const stock = flatMat(scene, "liteM40Composite", { albedo: [1.30, 1.30, 1.28], tex: texture, rough: .87, metal: 0 });
  const cheekMat = flatMat(scene, "liteM40CheekRest", { albedo: [.26, .26, .26], rough: .92, metal: 0 });
  stockLoft("liteM40Stock", parent, stock, stations);
  loft("liteM40RecoilPad", parent, rubber, [
    [-.491, .026, -.004, .092], [-.486, .031, -.010, .098],
    [-.475, .031, -.010, .102], [-.469, .028, -.006, .099],
  ]);
  stockLoft("liteM40CheekRest", parent, cheekMat, cheek);
}

export const buildLiteSniper = (scene) => buildSniperViewModel(scene, buildStock);

function clearance(p, outline) {
  if (p.z < outline[0][0] || p.z > outline.at(-1)[0]) return Infinity;
  const upper = outline.findIndex((row) => row[0] > p.z);
  const index = upper < 0 ? outline.length - 1 : upper;
  const a = outline[index - 1], b = outline[index];
  const t = (p.z - a[0]) / (b[0] - a[0]);
  const start = section(a), end = section(b);
  const ring = start.map((point, i) => Vector3.Lerp(point, end[i], t));
  let inside = false;
  let nearestSquared = Infinity;
  for (let i = 0; i < ring.length; i++) {
    const from = ring[i], to = ring[(i + 1) % ring.length];
    const dx = to.x - from.x, dy = to.y - from.y;
    const x = p.x - from.x, y = p.y - from.y;
    const t = Math.max(0, Math.min(1, (dx * x + dy * y) / (dx * dx + dy * dy)));
    nearestSquared = Math.min(nearestSquared, (x - t * dx) ** 2 + (y - t * dy) ** 2);
    if ((from.y > p.y) !== (to.y > p.y) && p.x < from.x + dx * (p.y - from.y) / dy) inside = !inside;
  }
  return (inside ? -1 : 1) * Math.sqrt(nearestSquared);
}

// Check surface vertices, edge midpoints and face centers of the animated
// bolt in stock space. The margin is measured within each cross-section.
export function sampleBoltClearance(model) {
  const group = model.pivots.boltGroup;
  // The bolt is parented directly to the stock's root. Compose child and
  // group matrices and cancel the viewmodel transform.
  const inverse = model.root.computeWorldMatrix(true).clone().invert();
  const point = new Vector3();
  const samples = [new Vector3(), new Vector3(), new Vector3()];
  let nearest = Infinity;
  for (const mesh of group.getChildMeshes()) {
    const vertices = mesh.getVerticesData("position");
    const indices = mesh.getIndices();
    if (!indices?.length || !vertices) continue;
    const conversion = mesh.computeWorldMatrix(true).multiply(inverse);
    for (let i = 0; i < indices.length; i += 3) {
      for (let k = 0; k < 3; k++) {
        const index = indices[i + k] * 3;
        Vector3.TransformCoordinatesFromFloatsToRef(vertices[index], vertices[index + 1], vertices[index + 2], conversion, samples[k]);
      }
      for (const sample of samples) nearest = Math.min(nearest, clearance(sample, stations), clearance(sample, cheek));
      for (let k = 0; k < 3; k++) {
        Vector3.LerpToRef(samples[k], samples[(k + 1) % 3], .5, point);
        nearest = Math.min(nearest, clearance(point, stations), clearance(point, cheek));
      }
      point.copyFrom(samples[0]).addInPlace(samples[1]).addInPlace(samples[2]).scaleInPlace(1 / 3);
      nearest = Math.min(nearest, clearance(point, stations), clearance(point, cheek));
    }
  }
  return nearest;
}
