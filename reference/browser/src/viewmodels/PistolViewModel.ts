import { Mesh } from "@babylonjs/core/Meshes/mesh";
import type { Scene } from "@babylonjs/core";
import { flatMat } from "../rendering/materials/canvas";
import {
  ALONG_Z,
  box,
  cyl,
  handAnchor,
  mergeWeaponParts,
  sharedBrassMat,
  sharedDarkTrimMat,
  sharedKnurlMat,
  loft,
  torus,
  triggerGuard,
} from "./kit";
import type { HandPose, WeaponViewModel } from "./kit";
import { polymerTexture, stippleTexture } from "./weaponTextures";

// USP .45 service pistol. Layout: +z is the muzzle direction; the sight line
// runs at y = +0.146 so the final ADS frame (root at y = -0.141) centers the
// sights on camera. Animated pivots the rig drives by name: slideGroup
// (blowback / lock-back), hammerGroup (rocks with the slide), magGroup
// (drops during reload).

// Firing hand high on the backstrap, thumb along the near flank
const RIGHT_GRIP: HandPose = {
  wrist: [0.02, 0.079, -0.213],
  knuckles: [0, -0.3, 0.95],
  palm: [-1, 0, 0],
  curl: { thumb: 0.1, index: [0.0, 0.9, 0.6], middle: 1.0, ring: 1.0, pinky: 1.0 },
};

// Support hand wrapped over the firing hand's fingers
const LEFT_GRIP: HandPose = {
  wrist: [-0.045, 0.041, -0.201],
  knuckles: [0.15, -0.25, 0.95],
  palm: [0.97, 0.1, -0.15],
  curl: { thumb: 0.4, index: 0.9, middle: 0.95, ring: 1.0, pinky: 1.0 },
};

export function buildPistolViewModel(scene: Scene): WeaponViewModel {
  const parent = new Mesh("usp45_root", scene);

  const polymerMat = flatMat(scene, "uspPolymerMat", { albedo: [1, 1, 1], rough: 0.58, tex: polymerTexture(scene) });
  const stippleMat = flatMat(scene, "uspStippleMat", { albedo: [1, 1, 1], rough: 0.75, tex: stippleTexture(scene) });
  const slideMat = flatMat(scene, "uspSlideMat", { albedo: [0.32, 0.34, 0.4], rough: 0.32, metal: 1 });
  const steelMat = flatMat(scene, "uspSteelMat", { albedo: [0.55, 0.57, 0.6], rough: 0.28, metal: 1 });
  const darkTrimMat = sharedDarkTrimMat(scene);
  const knurlMat = sharedKnurlMat(scene);
  const dotMat = flatMat(scene, "uspSightDotMat", { albedo: [0.85, 0.85, 0.8], rough: 0.6, emissive: [0.5, 0.5, 0.45] });
  const brassMat = sharedBrassMat(scene);

  // --- Slide group ---
  const slideGroup = new Mesh("slideGroup", scene);
  slideGroup.position.set(0, 0.118, 0);
  slideGroup.parent = parent;

  box("slideBody", [0.034, 0.034, 0.205], slideMat, slideGroup, null);
  cyl("slideSpine", { h: 0.19, d: 0.026, tess: 20 }, slideMat, slideGroup, [0, 0.011, 0], ALONG_Z);

  for (const [sx, sz, nm] of [
    [-0.0178, -0.072, "serRL"],
    [0.0178, -0.072, "serRR"],
    [-0.0178, 0.058, "serFL"],
    [0.0178, 0.058, "serFR"],
  ] as const) {
    box(nm, [0.0015, 0.026, 0.042], knurlMat, slideGroup, [sx, -0.001, sz]);
  }

  box("ejectionPort", [0.004, 0.018, 0.052], darkTrimMat, slideGroup, [0.0162, 0.006, 0.025]);
  box("extractor", [0.003, 0.005, 0.03], steelMat, slideGroup, [0.0168, 0.009, -0.012]);
  for (const ex of [-0.0172, 0.0172]) {
    box(`slideEtch${ex < 0 ? "L" : "R"}`, [0.0008, 0.002, 0.18], darkTrimMat, slideGroup, [ex, -0.009, 0]);
  }

  for (const ex of [-0.00825, 0.00825]) {
    box(`rearSightEar${ex < 0 ? "L" : "R"}`, [0.0085, 0.009, 0.013], slideMat, slideGroup, [ex, 0.0245, -0.094]);
  }
  box("rearSightBase", [0.025, 0.004, 0.013], slideMat, slideGroup, [0, 0.019, -0.094]);
  for (const dx of [-0.0075, 0.0075]) {
    cyl(`rearDot${dx < 0 ? "L" : "R"}`, { h: 0.0015, d: 0.0035, tess: 10 }, dotMat, slideGroup, [dx, 0.0255, -0.1008], {
      rx: Math.PI / 2,
    });
  }

  box("frontSight", [0.007, 0.011, 0.014], slideMat, slideGroup, [0, 0.024, 0.092]);
  cyl("frontDot", { h: 0.0015, d: 0.0035, tess: 10 }, dotMat, slideGroup, [0, 0.027, 0.0845], ALONG_Z);

  cyl("pistolBarrel", { h: 0.014, d: 0.0205, tess: 18 }, steelMat, slideGroup, [0, -0.0005, 0.108], ALONG_Z);
  cyl("muzzleBushing", { h: 0.006, d: 0.027, tess: 18 }, darkTrimMat, slideGroup, [0, -0.0005, 0.1045], ALONG_Z);
  cyl("pistolBore", { h: 0.003, d: 0.012, tess: 12 }, darkTrimMat, slideGroup, [0, -0.0005, 0.1155], ALONG_Z);
  box("slideRearPlate", [0.03, 0.028, 0.004], darkTrimMat, slideGroup, [0, -0.001, -0.1035]);
  cyl("chamberBrass", { h: 0.024, d: 0.0115, tess: 12 }, brassMat, parent, [0.004, 0.112, 0.022], ALONG_Z);

  // --- Frame (polymer) ---
  box("frameBody", [0.03, 0.032, 0.135], polymerMat, parent, [0, 0.09, -0.012]);
  box("frameRailEdge", [0.032, 0.003, 0.135], darkTrimMat, parent, [0, 0.1035, -0.012]);
  box("accessoryRail", [0.03, 0.014, 0.052], polymerMat, parent, [0, 0.083, 0.052]);
  for (const gy of [0.0795, 0.0865]) {
    box(`railGroove${gy}`, [0.031, 0.0022, 0.052], darkTrimMat, parent, [0, gy, 0.052]);
  }

  triggerGuard("pistolTriggerGuard", [0.017, 0.048, 0.072], polymerMat, parent, [0, 0.082, -0.016]);
  box("pistolTrigger", [0.006, 0.025, 0.009], steelMat, parent, [0, 0.063, -0.008], { rx: 0.28 });

  // --- Grip (raked back ~17°) ---
  const GRIP_RAKE = 0.3;
  const grip = loft("gripCore", parent, polymerMat, [
    [-0.028, 0.014, -0.02, 0.021],
    [-0.017, 0.016, -0.025, 0.026],
    [0.066, 0.016, -0.024, 0.028],
    [0.085, 0.0125, -0.018, 0.04],
    [0.089, 0.01, -0.013, 0.04],
  ]);
  grip.position.z = -0.08;
  grip.rotation.x = GRIP_RAKE - Math.PI / 2;
  box("gripPanels", [0.0335, 0.075, 0.04], stippleMat, parent, [0, 0.022, -0.0765], { rx: GRIP_RAKE });

  // --- Controls ---
  cyl("magRelease", { h: 0.005, d: 0.0095, tess: 12 }, steelMat, parent, [-0.0175, 0.07, -0.043], { rz: Math.PI / 2 });
  box("slideRelease", [0.0025, 0.0065, 0.046], steelMat, parent, [-0.0168, 0.1, -0.035]);
  cyl("slideReleasePin", { h: 0.004, d: 0.0065, tess: 10 }, steelMat, parent, [-0.0168, 0.1, -0.013], { rz: Math.PI / 2 });
  box("safetyLever", [0.0025, 0.012, 0.018], steelMat, parent, [-0.0168, 0.103, -0.083], { rx: -0.5 });
  cyl("takedownPin", { h: 0.004, d: 0.0065, tess: 10 }, steelMat, parent, [0.0162, 0.094, -0.02], { rz: Math.PI / 2 });
  torus("lanyardLoop", { d: 0.01, t: 0.002, tess: 12 }, steelMat, parent, [0, -0.03, -0.105], ALONG_Z);

  // --- Hammer group ---
  const hammerGroup = new Mesh("hammerGroup", scene);
  hammerGroup.position.set(0, 0.1, -0.098);
  hammerGroup.parent = parent;
  box("hammerBody", [0.011, 0.028, 0.007], steelMat, hammerGroup, [0, 0.008, -0.002]);
  cyl("hammerSpur", { h: 0.009, d: 0.019, tess: 14 }, steelMat, hammerGroup, [0, 0.0235, -0.008], { rz: Math.PI / 2 });
  cyl("hammerHole", { h: 0.0096, d: 0.0085, tess: 12 }, darkTrimMat, hammerGroup, [0, 0.0235, -0.008], { rz: Math.PI / 2 });
  hammerGroup.rotation.x = -0.55;
  cyl("hammerPin", { h: 0.036, d: 0.006, tess: 10 }, steelMat, parent, [0, 0.1, -0.098], { rz: Math.PI / 2 });

  // --- Magazine group ---
  const magGroup = new Mesh("magGroup", scene);
  magGroup.position.set(0, -0.022, -0.091);
  magGroup.rotation.x = GRIP_RAKE;
  magGroup.parent = parent;
  box("magBody", [0.0245, 0.055, 0.034], steelMat, magGroup, [0, 0.024, 0]);
  box("magBasePlate", [0.0315, 0.011, 0.046], polymerMat, magGroup, [0, -0.004, -0.001]);
  cyl("magTopRound", { h: 0.022, d: 0.0115, tess: 12 }, brassMat, magGroup, [0, 0.055, 0.002], ALONG_Z);

  mergeWeaponParts(parent, [slideGroup, hammerGroup, magGroup]);

  return {
    root: parent,
    pivots: { slideGroup, hammerGroup, magGroup },
    hands: { right: handAnchor("usp45_handR", parent, RIGHT_GRIP), left: handAnchor("usp45_handL", parent, LEFT_GRIP) },
  };
}
