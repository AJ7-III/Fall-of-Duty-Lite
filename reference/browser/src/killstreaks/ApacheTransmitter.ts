import { MeshBuilder } from "@babylonjs/core/Meshes/meshBuilder";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode";
import type { FreeCamera, Scene } from "@babylonjs/core";
import { stdMat } from "../rendering/materials/canvas";
import { CameraDevice, placePiece, smoothstep } from "./CameraDevice";

type TransmitterPhase = "away" | "raising" | "arming" | "lowering";

const RAISE_TIME = 0.22;
const ARM_TIME = 0.52;
const LOWER_TIME = 0.24;

// A quick handheld call-in controller: it rises into frame, the thumb presses
// the transmit button, then it drops away as the Apache starts its run-in.
export class ApacheTransmitter extends CameraDevice {
  private button: TransformNode;
  private phase: TransmitterPhase = "away";
  private signalSent = false;

  constructor(scene: Scene, camera: FreeCamera) {
    super("apacheTransmitter", scene);

    const casing = stdMat(scene, "apacheTxCasingMat", { diffuse: [0.09, 0.11, 0.09], spec: [0.06, 0.06, 0.05] });
    const rubber = stdMat(scene, "apacheTxRubberMat", { diffuse: [0.025, 0.028, 0.026], spec: [0, 0, 0] });
    const amber = stdMat(scene, "apacheTxAmberMat", {
      diffuse: [0.8, 0.48, 0.08],
      emissive: [0.28, 0.13, 0.02],
      spec: [0, 0, 0],
    });
    const skin = stdMat(scene, "apacheTxHandMat", { diffuse: [0.56, 0.38, 0.27], spec: [0.05, 0.035, 0.025] });

    placePiece(
      MeshBuilder.CreateBox("apacheTxBody", { width: 0.2, height: 0.3, depth: 0.075 }, scene),
      casing,
      this.root,
      0.04,
      0.02,
      0
    );
    placePiece(
      MeshBuilder.CreateBox("apacheTxGrip", { width: 0.16, height: 0.16, depth: 0.09 }, scene),
      rubber,
      this.root,
      0.04,
      -0.18,
      0
    );
    placePiece(
      MeshBuilder.CreateBox("apacheTxScreen", { width: 0.13, height: 0.06, depth: 0.008 }, scene),
      amber,
      this.root,
      0.04,
      0.09,
      -0.042
    );
    placePiece(
      MeshBuilder.CreateCylinder("apacheTxAntenna", { diameter: 0.012, height: 0.33, tessellation: 8 }, scene),
      rubber,
      this.root,
      0.12,
      0.28,
      0,
      0,
      0,
      -0.22
    );

    this.button = new TransformNode("apacheTxButton", scene);
    this.button.parent = this.root;
    this.button.position.set(-0.03, -0.02, 0);
    this.button.rotation.x = Math.PI / 2;
    placePiece(
      MeshBuilder.CreateCylinder("apacheTxButtonCap", { diameter: 0.055, height: 0.018, tessellation: 14 }, scene),
      amber,
      this.button,
      0,
      0,
      -0.045
    );

    // Minimal hand silhouette: a palm behind the unit and a thumb over the
    // button. Enough motion to read as "pressed by hand" without building a
    // full second set of arms.
    placePiece(
      MeshBuilder.CreateBox("apacheTxPalm", { width: 0.23, height: 0.13, depth: 0.1 }, scene),
      skin,
      this.root,
      -0.04,
      -0.18,
      0.035
    );
    placePiece(
      MeshBuilder.CreateCapsule("apacheTxThumb", { radius: 0.022, height: 0.17, tessellation: 8, capSubdivisions: 3 }, scene),
      skin,
      this.button,
      -0.035,
      0.006,
      -0.01,
      0,
      0,
      1.1
    );

    this.finalizeRig(camera, 0.28, -0.18, 0.06, [this.button]);
  }

  public get isOut(): boolean {
    return this.phase !== "away";
  }

  public open(): void {
    if (this.phase !== "away") return;
    this.phase = "raising";
    this.phaseT = 0;
    this.signalSent = false;
    this.root.setEnabled(true);
  }

  public forceClose(): void {
    this.phase = "away";
    this.phaseT = 0;
    this.signalSent = false;
    this.root.setEnabled(false);
  }

  // Returns true once, on the frame the transmitter sends the call-in signal.
  public update(dt: number): boolean {
    if (this.phase === "away") return false;

    this.phaseT += dt;
    let hold = 0;
    let sent = false;

    if (this.phase === "raising") {
      const s = smoothstep(this.phaseT / RAISE_TIME);
      this.root.position.set(0.18 - s * 0.06, -0.56 + s * 0.33, 0.48);
      if (this.phaseT >= RAISE_TIME) {
        this.phase = "arming";
        this.phaseT = 0;
      }
    } else if (this.phase === "arming") {
      hold = Math.sin(Math.min(1, this.phaseT / ARM_TIME) * Math.PI);
      if (!this.signalSent && this.phaseT >= ARM_TIME * 0.48) {
        this.signalSent = true;
        sent = true;
      }
      if (this.phaseT >= ARM_TIME) {
        this.phase = "lowering";
        this.phaseT = 0;
      }
    } else {
      const s = smoothstep(this.phaseT / LOWER_TIME);
      this.root.position.set(0.12 + s * 0.04, -0.23 - s * 0.34, 0.48);
      if (this.phaseT >= LOWER_TIME) {
        this.forceClose();
      }
    }

    this.button.position.z = -0.01 * hold;
    this.root.rotation.z = 0.06 + Math.sin(this.phaseT * 18) * 0.006;
    return sent;
  }
}
