import { Mesh } from "@babylonjs/core/Meshes/mesh";
import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { Vector3 } from "@babylonjs/core/Maths/math.vector";
import { revolve } from "../rendering/meshProfiles";
import type { Scene } from "@babylonjs/core";
import { flatMat } from "../rendering/materials/canvas";
import {
  ALONG_Z,
  box,
  cyl,
  handAnchor,
  mergeWeaponParts,
  prim,
  sharedBrassMat,
  sharedKnurlMat,
  sharedRubberMat,
  loft,
  triggerGuard,
} from "./kit";
import type { HandPose, WeaponViewModel } from "./kit";
import { mp44MetalTexture, mp44WoodTexture } from "./weaponTextures";

// MP44 assault rifle: hooded ring front sight, tall rear notch, ribbed
// stamped receiver, dark wet metal, curved magazine. Animated pivots the
// rig drives by name: mp44BoltGroup (reciprocates / charging handle),
// mp44MagGroup (drops and seats during reload).

// Trigger hand on the wooden pistol grip
// (The soldier's hand runs 0.163 m from wrist to middle knuckle at rig scale, so
// each wrist sits that far back from where the knuckles must land.)
const RIGHT_GRIP: HandPose = {
  wrist: [0.036, 0.014, -0.306],
  knuckles: [0, -0.15, 0.99],
  palm: [-1, 0, 0],
  curl: { thumb: 0.1, index: [0.12, 0.9, 0.55], middle: 1.0, ring: 1.0, pinky: 1.0 },
};

// Support hand cupping the fore-end from below
const LEFT_GRIP: HandPose = {
  wrist: [-0.117, -0.007, 0.079],
  knuckles: [0.9, 0.35, 0.25],
  palm: [-0.35, 0.92, 0.1],
  curl: { thumb: [0.1, -0.15, -0.2], index: 0.85, middle: 0.9, ring: 0.95, pinky: 1.0 },
};

export function buildMp44ViewModel(scene: Scene): WeaponViewModel {
  const parent = new Mesh("mp44_root", scene);

  const metalMat = flatMat(scene, "mp44MetalMat", {
    albedo: [2.0, 2.02, 2.06],
    rough: 0.46,
    metal: 0.55,
    tex: mp44MetalTexture(scene),
  });
  const darkMat = flatMat(scene, "mp44DarkMat", { albedo: [0.16, 0.17, 0.18], rough: 0.5, metal: 0.6 });
  const edgeMat = flatMat(scene, "mp44WornEdgeMat", { albedo: [0.62, 0.64, 0.66], rough: 0.24, metal: 1 });
  const woodMat = flatMat(scene, "mp44WoodMat", { albedo: [1.35, 1.22, 1.08], rough: 0.42, tex: mp44WoodTexture(scene) });
  const knurlMat = sharedKnurlMat(scene);
  const sightDotMat = flatMat(scene, "mp44SightDotMat", { albedo: [0.72, 0.72, 0.66], rough: 0.6, emissive: [0.32, 0.32, 0.26] });
  const brassMat = sharedBrassMat(scene);
  const rubberMat = sharedRubberMat(scene);

  // --- Receiver and barrel assembly ---
  box("mp44Receiver", [0.062, 0.072, 0.34], metalMat, parent, [0, 0.103, -0.055]);
  // Kept below the y=+0.17 sight axis so nothing crosses the ADS view
  cyl("mp44ReceiverTop", { h: 0.31, d: 0.05, tess: 24 }, metalMat, parent, [0, 0.128, -0.047], { rx: Math.PI / 2, sx: 0.74 });
  box("mp44LowerFold", [0.067, 0.013, 0.31], darkMat, parent, [0, 0.064, -0.055]);

  for (let i = 0; i < 7; i++) {
    box(`mp44ReceiverRib${i}`, [0.066, 0.0045, 0.012], edgeMat, parent, [0, 0.1285, -0.18 + i * 0.046]);
  }
  for (const sx of [-1, 1]) {
    box(`mp44SideStamp${sx}`, [0.003, 0.027, 0.19], darkMat, parent, [0.033 * sx, 0.111, -0.045]);
    for (let i = 0; i < 6; i++) {
      cyl(
        `mp44Rivet${sx}_${i}`,
        { h: 0.004, d: 0.0062, tess: 10 },
        edgeMat,
        parent,
        [0.035 * sx, 0.077 + (i % 2) * 0.047, -0.178 + i * 0.061],
        {
          rz: Math.PI / 2,
        }
      );
    }
  }

  box("mp44EjectionPort", [0.004, 0.026, 0.068], darkMat, parent, [-0.0338, 0.124, 0.025]);

  const boltGroup = new Mesh("mp44BoltGroup", scene);
  boltGroup.position.set(-0.037, 0.126, 0.026);
  boltGroup.parent = parent;
  box("mp44BoltFace", [0.005, 0.02, 0.054], edgeMat, boltGroup, [0.001, 0, 0]);
  cyl("mp44ChargeStem", { h: 0.026, d: 0.006, tess: 10 }, edgeMat, boltGroup, [-0.014, 0.004, -0.017], { rz: Math.PI / 2 });
  cyl("mp44ChargeKnob", { h: 0.012, top: 0.016, bot: 0.019, tess: 16 }, knurlMat, boltGroup, [-0.031, 0.004, -0.017], {
    rz: Math.PI / 2,
  });

  cyl("mp44ChamberRound", { h: 0.03, d: 0.0095, tess: 12 }, brassMat, parent, [-0.01, 0.126, 0.025], ALONG_Z);
  cyl("mp44GasTube", { h: 0.27, d: 0.024, tess: 24 }, metalMat, parent, [0, 0.146, 0.18], ALONG_Z);
  cyl("mp44GasBlock", { h: 0.03, d: 0.032, tess: 20 }, metalMat, parent, [0, 0.14, 0.318], ALONG_Z);
  cyl("mp44GasPort", { h: 0.0185, d: 0.007, tess: 12 }, darkMat, parent, [0, 0.153, 0.318]);
  cyl("mp44Barrel", { h: 0.36, top: 0.016, bot: 0.021, tess: 24 }, edgeMat, parent, [0, 0.121, 0.285], ALONG_Z);

  for (const z of [0.08, 0.16, 0.255, 0.37]) {
    cyl(`mp44BarrelBand${z}`, { h: 0.012, d: 0.034, tess: 20 }, darkMat, parent, [0, 0.134, z], ALONG_Z);
  }

  cyl("mp44Muzzle", { h: 0.034, d: 0.024, tess: 20 }, darkMat, parent, [0, 0.121, 0.475], ALONG_Z);
  cyl("mp44MuzzleCrown", { h: 0.006, top: 0.025, bot: 0.02, tess: 20 }, edgeMat, parent, [0, 0.121, 0.49], { rx: -Math.PI / 2 });
  cyl("mp44Bore", { h: 0.004, d: 0.011, tess: 12 }, darkMat, parent, [0, 0.121, 0.494], ALONG_Z);
  cyl("mp44MuzzleNut", { h: 0.011, d: 0.0265, tess: 20 }, knurlMat, parent, [0, 0.121, 0.458], ALONG_Z);

  // --- Iron sights (sight axis at x=0, y=+0.17) ---
  const sightY = 0.17;
  cyl("mp44FrontSightBase", { h: 0.032, bot: 0.036, top: 0.013, tess: 4 }, metalMat, parent, [0, 0.137, 0.429], {
    ry: Math.PI / 4,
  });
  const hood = revolve(
    scene,
    "mp44FrontSightHood",
    36,
    [
      [0.021, -0.004],
      [0.024, -0.004],
      [0.025, -0.002],
      [0.025, 0.003],
      [0.0235, 0.005],
      [0.021, 0.005],
    ],
    true
  );
  prim(hood, darkMat, parent, [0, sightY, 0.429], ALONG_Z);
  cyl("mp44FrontPost", { h: 0.0145, bot: 0.006, top: 0.0044, tess: 10 }, darkMat, parent, [0, 0.159, 0.429]);
  cyl("mp44FrontPostTip", { h: 0.0037, bot: 0.0044, top: 0.0036, tess: 10 }, sightDotMat, parent, [0, sightY - 0.00185, 0.429]);

  box("mp44RearSightRamp", [0.04, 0.016, 0.055], metalMat, parent, [0, 0.147, -0.148], { rx: -0.08 });
  box("mp44RearSlider", [0.045, 0.007, 0.015], edgeMat, parent, [0, 0.1545, -0.138]);
  box("mp44RearLeaf", [0.044, 0.009, 0.0055], darkMat, parent, [0, 0.15, -0.168]);
  for (const sx of [-1, 1]) {
    box(`mp44RearCheek${sx}`, [0.0165, 0.011, 0.0055], darkMat, parent, [0.0125 * sx, 0.157, -0.168], { rz: 0.42 * sx });
    box(`mp44RearCheekWear${sx}`, [0.014, 0.0016, 0.0058], edgeMat, parent, [0.0104 * sx, 0.1621, -0.168], { rz: 0.42 * sx });
  }

  // --- Furniture: dark wood stock, pistol grip, fore-end ---
  loft("mp44ButtPad", parent, rubberMat, [
    [-0.474, 0.029, 0.02, 0.114],
    [-0.469, 0.033, 0.016, 0.119],
    [-0.449, 0.032, 0.018, 0.117],
  ]);
  loft("mp44Stock", parent, woodMat, [
    [-0.449, 0.032, 0.018, 0.117],
    [-0.421, 0.033, 0.021, 0.121],
    [-0.329, 0.028, 0.034, 0.122],
    [-0.259, 0.022, 0.055, 0.11],
    [-0.215, 0.025, 0.058, 0.119],
    [-0.179, 0.031, 0.057, 0.122],
  ]);
  box("mp44PistolGrip", [0.052, 0.13, 0.045], woodMat, parent, [0, 0.012, -0.162], { rx: 0.36 });
  box("mp44GripCap", [0.057, 0.011, 0.052], darkMat, parent, [0, -0.054, -0.184], { rx: 0.36 });
  cyl("mp44ForeEnd", { h: 0.225, top: 0.063, bot: 0.073, tess: 22 }, woodMat, parent, [0, 0.067, 0.106], {
    rx: Math.PI / 2,
    sx: 0.72,
  });
  for (let i = 0; i < 4; i++) {
    box(`mp44ForeGroove${i}`, [0.052, 0.003, 0.17], darkMat, parent, [0, 0.091 + i * 0.006, 0.112]);
  }

  // Trigger group and controls
  triggerGuard("mp44TriggerGuard", [0.013, 0.055, 0.071], metalMat, parent, [0, 0.067, -0.119]);
  box("mp44Trigger", [0.007, 0.031, 0.011], edgeMat, parent, [0, 0.046, -0.105], { rx: 0.22 });
  cyl("mp44Selector", { h: 0.006, d: 0.014, tess: 14 }, knurlMat, parent, [-0.035, 0.111, -0.125], { rz: Math.PI / 2 });
  box("mp44SelectorLever", [0.004, 0.007, 0.032], edgeMat, parent, [-0.038, 0.104, -0.108], { rx: -0.45 });
  cyl("mp44SafetyButton", { h: 0.006, d: 0.0105, tess: 12 }, edgeMat, parent, [-0.034, 0.07, -0.118], { rz: Math.PI / 2 });

  // --- Curved stamped magazine group ---
  const magGroup = new Mesh("mp44MagGroup", scene);
  magGroup.position.set(0, 0.028, -0.035);
  magGroup.rotation.x = -0.1;
  magGroup.parent = parent;

  const magW = 0.026,
    magD = 0.029,
    magCh = 0.007,
    magRib = 0.0018;
  const magSide = [
    [magW - magCh, -magD],
    [magW, magCh - magD],
    [magW, -0.013],
    [magW + magRib, -0.0095],
    [magW, -0.006],
    [magW, 0.006],
    [magW + magRib, 0.0095],
    [magW, 0.013],
    [magW, magD - magCh],
    [magW - magCh, magD],
  ].map(([x, y]) => new Vector3(x, y, 0));
  const magShape = magSide.concat(
    magSide
      .slice()
      .reverse()
      .map((p) => new Vector3(-p.x, p.y, 0))
  );
  const magPath: Vector3[] = [];
  for (let i = 0; i <= 10; i++) {
    const a = 0.05 + (i / 10) * 0.34;
    magPath.push(new Vector3(0, 0.012 - 0.5 * (Math.sin(a) - Math.sin(0.05)), 0.004 + 0.5 * (Math.cos(0.05) - Math.cos(a))));
  }
  const magBody = MeshBuilder.ExtrudeShape(
    "mp44MagBody",
    { shape: magShape, path: magPath, closeShape: true, cap: Mesh.CAP_ALL },
    scene
  );
  magBody.material = metalMat;
  magBody.parent = magGroup;

  box("mp44MagFeedLips", [0.046, 0.012, 0.052], edgeMat, magGroup, [0, 0.011, 0.002]);
  cyl("mp44MagTopRound", { h: 0.032, d: 0.0095, tess: 12 }, brassMat, magGroup, [0, 0.021, 0.006], ALONG_Z);
  box("mp44MagBasePlate", [0.058, 0.012, 0.066], darkMat, magGroup, [0, -0.157, 0.043], { rx: -0.39 });

  mergeWeaponParts(parent, [boltGroup, magGroup]);

  return {
    root: parent,
    pivots: { boltGroup, magGroup },
    hands: { right: handAnchor("mp44_handR", parent, RIGHT_GRIP), left: handAnchor("mp44_handL", parent, LEFT_GRIP) },
  };
}
