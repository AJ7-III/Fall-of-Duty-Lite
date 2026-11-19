import { Color3 } from "@babylonjs/core/Maths/math.color";
import { FresnelParameters } from "@babylonjs/core/Materials/fresnelParameters";
import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { Texture } from "@babylonjs/core/Materials/Textures/texture";
import type { Scene } from "@babylonjs/core";
import { cachedTexture, flatMat, makeCanvasTexture, stdMat } from "../rendering/materials/canvas";
import { revolve } from "../rendering/meshProfiles";
import {
  ALONG_Z,
  box,
  cyl,
  handAnchor,
  mergeWeaponParts,
  prim,
  sharedDarkTrimMat,
  sharedKnurlMat,
  sharedRubberMat,
  loft,
  torus,
  triggerGuard,
} from "./kit";
import type { HandPose, WeaponViewModel } from "./kit";
import { camoTexture } from "./weaponTextures";

// M40A3 bolt-action rifle. Layout: +z is the muzzle direction. The scope
// tube axis sits at exactly y = +0.17 / x = 0 so the final ADS frame (root
// at y = -0.17) centers it on camera. Animated pivot the rig drives by name:
// boltGroup (rotates open, pulls back, runs home).

const RIGHT_GRIP: HandPose = {
  wrist: [0.04, 0.098, -0.365],
  knuckles: [0, -0.6, 0.8],
  palm: [-1, 0, 0],
  curl: { thumb: 0.1, index: [0.1, 0.9, 0.5], middle: 1.0, ring: 1.0, pinky: 1.0 },
};

const RIGHT_BOLT: HandPose = {
  wrist: [0.157, 0.139, -0.26],
  knuckles: [-0.75, -0.35, 0.55],
  palm: [-0.3, -0.85, -0.4],
  curl: { thumb: 0.6, index: 0.8, middle: 0.9, ring: 0.9, pinky: 0.9 },
};

const LEFT_GRIP: HandPose = {
  wrist: [-0.092, -0.037, 0.089],
  knuckles: [0.9, 0.35, 0.25],
  palm: [-0.35, 0.92, 0.1],
  curl: { thumb: [0, 0.5, -0.2], index: 0.55, middle: 0.55, ring: 0.55, pinky: 0.38 },
};

type SniperStockBuilder = (parent: Mesh, rubber: ReturnType<typeof sharedRubberMat>) => void;

export function buildSniperViewModel(scene: Scene, buildStock?: SniperStockBuilder): WeaponViewModel {
  const parent = new Mesh("m40a3_root", scene);

  const metalMat = flatMat(scene, "sniperMetalMat", { albedo: [0.3, 0.32, 0.38], rough: 0.3, metal: 1 });
  const scopeMat = flatMat(scene, "sniperScopeMat", { albedo: [0.1, 0.1, 0.11], rough: 0.45, metal: 0.9 });
  const knurlMat = sharedKnurlMat(scene);
  const darkTrimMat = sharedDarkTrimMat(scene);
  const rubberMat = sharedRubberMat(scene);

  const lensReflTex = cachedTexture(scene, "lensReflTex", () => {
    const tex = makeCanvasTexture(scene, "lensReflTex", 256, (ctx, s) => {
      const grad = ctx.createLinearGradient(0, 0, 0, s);
      grad.addColorStop(0.0, "#dcebf7");
      grad.addColorStop(0.45, "#94aabf");
      grad.addColorStop(0.6, "#eef6fb");
      grad.addColorStop(0.68, "#62788c");
      grad.addColorStop(1.0, "#27333f");
      ctx.fillStyle = grad;
      ctx.fillRect(0, 0, s, s);
      const sun = ctx.createRadialGradient(s * 0.68, s * 0.22, 2, s * 0.68, s * 0.22, 46);
      sun.addColorStop(0, "rgba(255,255,248,0.95)");
      sun.addColorStop(1, "rgba(255,255,248,0)");
      ctx.fillStyle = sun;
      ctx.beginPath();
      ctx.arc(s * 0.68, s * 0.22, 46, 0, Math.PI * 2);
      ctx.fill();
    });
    tex.coordinatesMode = Texture.SPHERICAL_MODE;
    return tex;
  });

  const lensMat = stdMat(scene, "sniperLensMat", {
    diffuse: [0.01, 0.015, 0.02],
    spec: [1, 1, 1],
    power: 128,
    emissive: [0.38, 0.45, 0.5],
  });
  lensMat.reflectionTexture = lensReflTex;
  lensMat.reflectionFresnelParameters = new FresnelParameters({ bias: 0.25, power: 1.4 });
  lensMat.emissiveFresnelParameters = new FresnelParameters({
    bias: 0.12,
    power: 2.2,
    leftColor: new Color3(0.45, 0.24, 0.6),
    rightColor: new Color3(0.08, 0.3, 0.36),
  });

  const lensRearMat = stdMat(scene, "sniperLensRearMat", {
    diffuse: [0.01, 0.01, 0.015],
    spec: [0.6, 0.65, 0.7],
    emissive: [0.03, 0.05, 0.06],
  });
  lensRearMat.reflectionTexture = lensReflTex;
  lensRearMat.reflectionFresnelParameters = new FresnelParameters({ bias: 0.06, power: 3 });

  // --- Stock ---
  if (buildStock) buildStock(parent, rubberMat);
  else {
    const stockMat = flatMat(scene, "sniperStockMat", { albedo: [1, 1, 1], rough: 0.55, tex: camoTexture(scene) });
    prim(
      MeshBuilder.CreateCapsule("stockButtPad", { height: 0.115, radius: 0.027, tessellation: 16, capSubdivisions: 6 }, scene),
      rubberMat,
      parent,
      [0, 0.05, -0.473],
      { sz: 0.62 }
    );
    loft("sniperStock", parent, stockMat, [
      [-0.468, 0.024, 0.002, 0.099],
      [-0.447, 0.028, -0.004, 0.107],
      [-0.383, 0.03, 0.007, 0.136],
      [-0.3, 0.029, 0.018, 0.136],
      [-0.258, 0.024, 0.013, 0.114],
      [-0.22, 0.023, 0.002, 0.107],
      [-0.181, 0.032, 0.026, 0.11],
      [0.136, 0.029, 0.037, 0.105],
      [0.207, 0.025, 0.04, 0.096],
      [0.222, 0.021, 0.043, 0.089],
    ]);
  }

  // --- Action / barrel ---
  const receiver = revolve(scene, "receiver", 24, [
    [0, -0.268],
    [0.011, -0.268],
    [0.014, -0.265],
    [0.015, -0.24],
    [0.025, -0.217],
    [0.025, 0.014],
    [0.02, 0.02],
    [0, 0.02],
  ]);
  prim(receiver, metalMat, parent, [0, 0.115, 0], ALONG_Z);
  cyl("barrel", { h: 0.26, top: 0.022, bot: 0.035, tess: 24 }, metalMat, parent, [0, 0.115, 0.15], ALONG_Z);
  cyl("muzzle", { h: 0.02, d: 0.0255, tess: 16 }, scopeMat, parent, [0, 0.115, 0.288], ALONG_Z);
  cyl("muzzleBore", { h: 0.004, d: 0.013, tess: 12 }, darkTrimMat, parent, [0, 0.115, 0.299], ALONG_Z);
  box("magPlate", [0.034, 0.008, 0.105], metalMat, parent, [0, 0.028, -0.105]);
  triggerGuard("triggerGuard", [0.016, 0.061, 0.061], metalMat, parent, [0, 0.028, -0.213]);
  box("trigger", [0.007, 0.028, 0.01], metalMat, parent, [0, 0.004, -0.206], { rx: 0.25 });

  // --- Scope rail & ring mounts ---
  box("scopeRail", [0.022, 0.01, 0.18], scopeMat, parent, [0, 0.142, -0.07]);
  for (const z of [0, -0.14]) {
    box(`scopeMount${z}`, [0.018, 0.026, 0.024], scopeMat, parent, [0, 0.152, z]);
    cyl(`scopeRingClamp${z}`, { h: 0.016, d: 0.041, tess: 20 }, scopeMat, parent, [0, 0.17, z], ALONG_Z);
    cyl(`scopeScrew${z}`, { h: 0.006, d: 0.007, tess: 10 }, metalMat, parent, [-0.0215, 0.156, z], { rz: Math.PI / 2 });
  }

  // --- Scope (axis exactly at y = +0.17) ---
  const SCOPE_Y = 0.17;
  const housing = revolve(scene, "scopeHousing", 24, [
    [0.019, -0.305],
    [0.024, -0.308],
    [0.026, -0.29],
    [0.026, -0.27],
    [0.017, -0.21],
    [0.017, -0.093],
    [0.0235, -0.085],
    [0.0235, -0.015],
    [0.017, -0.007],
    [0.017, 0.03],
    [0.032, 0.092],
    [0.034, 0.1],
    [0.034, 0.139],
    [0.036, 0.144],
    [0.034, 0.152],
    [0.0295, 0.15],
    [0.0295, 0.132],
  ]);
  prim(housing, scopeMat, parent, [0, SCOPE_Y, 0], ALONG_Z);

  for (const [name, x, y, diameter, depth] of [
    ["turretTop", 0, 1, 0.031, 0.022],
    ["turretSide", 1, 0, 0.031, 0.022],
    ["parallax", -1, 0, 0.036, 0.026],
  ] as const) {
    const position = (distance: number) => [x * distance, SCOPE_Y + y * distance, -0.05] as const;
    const rotation = { rz: (-x * Math.PI) / 2 };
    cyl(`${name}Base`, { h: 0.012, d: diameter + 0.004, tess: 16 }, scopeMat, parent, position(0.0295), rotation);
    cyl(name, { h: depth, d: diameter, tess: 16 }, knurlMat, parent, position(0.035 + depth / 2), rotation);
    cyl(`${name}Cap`, { h: 0.005, d: diameter - 0.001, tess: 16 }, scopeMat, parent, position(0.037 + depth), rotation);
    cyl(
      `${name}Index`,
      { h: 0.002, d: 0.003, tess: 8 },
      metalMat,
      parent,
      [x * 0.046, SCOPE_Y + y * 0.046, -0.05 + diameter / 2],
      ALONG_Z
    );
  }

  cyl("scopeFocusRing", { h: 0.024, d: 0.054, tess: 24 }, knurlMat, parent, [0, SCOPE_Y, -0.282], ALONG_Z);
  torus("scopeEyecup", { d: 0.044, t: 0.0065, tess: 24 }, rubberMat, parent, [0, SCOPE_Y, -0.307], ALONG_Z);
  for (const [name, z, radius, direction, mat] of [
    ["scopeLensFront", 0.136, 0.03, 1, lensMat],
    ["scopeLensRear", -0.301, 0.02, -1, lensRearMat],
  ] as const) {
    const lens = revolve(scene, name, 24, [
      [radius, 0],
      [radius * 0.7, 0.0035],
      [0, 0.006],
    ]);
    prim(lens, mat, parent, [0, SCOPE_Y, z], { rx: (direction * Math.PI) / 2 });
  }

  // --- Bolt assembly ---
  const boltGroup = new Mesh("boltGroup", scene);
  boltGroup.position.set(0, 0.125, -0.16);
  boltGroup.parent = parent;
  cyl("boltShaft", { h: 0.1, d: 0.018, tess: 16 }, metalMat, boltGroup, [0, 0, 0.01], ALONG_Z);
  cyl("boltArm", { h: 0.05, top: 0.011, bot: 0.015, tess: 12 }, metalMat, boltGroup, [0.027, 0, -0.01], { rz: -Math.PI / 2 });
  cyl("boltKnob", { h: 0.023, top: 0.025, bot: 0.031, tess: 16 }, metalMat, boltGroup, [0.055, 0, -0.01], {
    rz: -Math.PI / 2,
  });
  boltGroup.rotation.z = -0.9;

  mergeWeaponParts(parent, [boltGroup]);

  return {
    root: parent,
    pivots: { boltGroup },
    hands: {
      right: handAnchor("m40a3_handR", parent, RIGHT_GRIP, RIGHT_BOLT),
      left: handAnchor("m40a3_handL", parent, LEFT_GRIP),
    },
  };
}
