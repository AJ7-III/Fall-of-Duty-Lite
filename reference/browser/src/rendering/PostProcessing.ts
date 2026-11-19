import "@babylonjs/core/PostProcesses/RenderPipeline/postProcessRenderPipelineManagerSceneComponent";
import "@babylonjs/core/Rendering/prePassRendererSceneComponent";
import "@babylonjs/core/Rendering/geometryBufferRendererSceneComponent";
import "@babylonjs/core/Rendering/depthRendererSceneComponent";
import { ColorCurves } from "@babylonjs/core/Materials/colorCurves";
import { DefaultRenderingPipeline } from "@babylonjs/core/PostProcesses/RenderPipeline/Pipelines/defaultRenderingPipeline";
import { ImageProcessingConfiguration } from "@babylonjs/core/Materials/imageProcessingConfiguration";
import { SSAO2RenderingPipeline } from "@babylonjs/core/PostProcesses/RenderPipeline/Pipelines/ssao2RenderingPipeline";
import type { Camera, Engine, Scene } from "@babylonjs/core";
import { Settings } from "../ui/Settings";
import type { GraphicsQuality } from "../ui/Settings";

// The image pipeline: resolution, anti-aliasing, ambient occlusion,
// sharpening, tone mapping and the film look — packaged as three quality
// tiers with an automatic step-down when the frame rate can't hold.
//
//   high        native device pixels (capped at 2x), 4x MSAA, SSAO, sharpen
//   balanced    1.5x pixels max, 2x MSAA, sharpen
//   performance 1x pixels / 1080p pixel budget, FXAA, 60 FPS, no cosmetic passes
//
// FXAA alone (the old setup) softens every texel; MSAA resolves geometry
// edges without touching texture detail, and the sharpen pass restores the
// micro-contrast the tone mapper and bloom take away.

interface Tier {
  maxPixelRatio: number;
  samples: number;
  fxaa: boolean;
  ssao: boolean;
  sharpen: number;
  maxPixels?: number;
  maxFps?: number;
}

const TIERS: Record<GraphicsQuality, Tier> = {
  high: { maxPixelRatio: 2, samples: 4, fxaa: false, ssao: true, sharpen: 0.32 },
  balanced: { maxPixelRatio: 1.5, samples: 2, fxaa: false, ssao: false, sharpen: 0.28 },
  performance: { maxPixelRatio: 1, samples: 1, fxaa: true, ssao: false, sharpen: 0, maxPixels: 1920 * 1080, maxFps: 60 },
};

const TIER_ORDER: GraphicsQuality[] = ["high", "balanced", "performance"];

export class PostProcessing {
  private engine: Engine;
  private scene: Scene;
  private camera: Camera;
  private pipeline: DefaultRenderingPipeline;
  private ssao: SSAO2RenderingPipeline | null = null;
  private quality: GraphicsQuality;
  private warmupSamples = 2;

  // Adaptive step-down bookkeeping: rolling average over the last seconds
  private fpsSamples: number[] = [];
  private onQualityChange: ((q: GraphicsQuality, auto: boolean) => void) | null = null;

  constructor(engine: Engine, scene: Scene, camera: Camera) {
    this.engine = engine;
    this.scene = scene;
    this.camera = camera;
    this.quality = Settings.getGraphicsQuality();

    const p = new DefaultRenderingPipeline("postfx", true, scene, [camera]);
    p.imageProcessing.toneMappingEnabled = true;
    p.imageProcessing.toneMappingType = ImageProcessingConfiguration.TONEMAPPING_ACES;
    p.imageProcessing.exposure = 1.18;
    p.imageProcessing.vignetteEnabled = true;
    p.imageProcessing.vignetteWeight = 0.8;
    p.imageProcessing.vignetteStretch = 0.3;
    // Grade: cool, slightly desaturated shadows and a warm lift in the
    // highlights — the split tone of an overcast day under sodium lamps
    const curves = new ColorCurves();
    curves.globalSaturation = -6;
    curves.shadowsHue = 215;
    curves.shadowsSaturation = 12;
    curves.shadowsDensity = 35;
    curves.midtonesHue = 200;
    curves.midtonesSaturation = 4;
    curves.midtonesDensity = 15;
    curves.highlightsHue = 40;
    curves.highlightsSaturation = 10;
    curves.highlightsDensity = 30;
    p.imageProcessing.colorCurvesEnabled = true;
    p.imageProcessing.colorCurves = curves;
    const cosmeticEffects = this.quality !== "performance";
    p.bloomEnabled = cosmeticEffects;
    p.bloomThreshold = 0.86;
    p.bloomWeight = 0.14;
    p.bloomKernel = 48;
    p.bloomScale = 0.5;
    p.grainEnabled = cosmeticEffects;
    p.grain.intensity = 2;
    p.grain.animated = true;
    p.sharpenEnabled = cosmeticEffects;
    p.sharpen.colorAmount = 1.0;
    this.pipeline = p;

    this.apply();
  }

  public get currentQuality(): GraphicsQuality {
    return this.quality;
  }

  public get frameRateLimit(): number | undefined {
    return TIERS[this.quality].maxFps;
  }

  public setOnQualityChange(cb: (q: GraphicsQuality, auto: boolean) => void): void {
    this.onQualityChange = cb;
  }

  public setQuality(q: GraphicsQuality, persist: boolean = true): void {
    // Clicking the active automatic tier still makes it an explicit choice.
    if (persist) Settings.setGraphicsQuality(q);
    if (q === this.quality) return;
    this.quality = q;
    this.resetPerformanceSamples();
    this.apply();
    this.onQualityChange?.(q, !persist);
  }

  public resetPerformanceSamples(): void {
    this.fpsSamples.length = 0;
    this.warmupSamples = 2;
  }

  // Ignore shader warm-up/resume, then evaluate five seconds of actual play.
  // Each step gets a fresh settling period before considering another tier.
  public reportFps(fps: number): void {
    if (fps <= 0) return;
    if (this.warmupSamples > 0) {
      this.warmupSamples--;
      return;
    }
    this.fpsSamples.push(fps);
    if (this.fpsSamples.length < 5) return;
    const avg = this.fpsSamples.reduce((a, b) => a + b, 0) / this.fpsSamples.length;
    this.fpsSamples.length = 0;
    const i = TIER_ORDER.indexOf(this.quality);
    if (avg < 45 && i < TIER_ORDER.length - 1) {
      const next = TIER_ORDER[i + 1];
      this.setQuality(next, false);
      Settings.setAutoGraphicsQuality(next); // the next load starts here, unless the player has picked a tier
    }
  }

  public resize(): void {
    const tier = TIERS[this.quality];
    let dpr = Math.min(window.devicePixelRatio || 1, tier.maxPixelRatio);
    if (tier.maxPixels) {
      const canvas = this.engine.getRenderingCanvas();
      const width = Math.max(1, canvas?.clientWidth || window.innerWidth);
      const height = Math.max(1, canvas?.clientHeight || window.innerHeight);
      dpr = Math.min(dpr, Math.sqrt(tier.maxPixels / (width * height)));
    }
    // This setter also resizes the backing canvas.
    this.engine.setHardwareScalingLevel(1 / dpr);
    // bloom is a screen-space blur: keep its footprint constant in pixels
    this.pipeline.bloomKernel = Math.round(48 * Math.max(1, (1 / this.engine.getHardwareScalingLevel()) * 0.75));
  }

  private apply(): void {
    const tier = TIERS[this.quality];
    this.resize();

    const p = this.pipeline;
    p.samples = tier.samples;
    p.fxaaEnabled = tier.fxaa;
    const cosmeticEffects = this.quality !== "performance";
    p.bloomEnabled = cosmeticEffects;
    p.grainEnabled = cosmeticEffects;
    p.sharpenEnabled = tier.sharpen > 0;
    p.sharpen.edgeAmount = tier.sharpen;

    if (tier.ssao && !this.ssao) {
      // Ambient occlusion: contact shadow where containers meet the grass,
      // crates meet the ground, the rifle meets the hands. Half-res AO
      // buffer with a bilateral blur — the "grounding" the flat ambient
      // light otherwise lacks.
      const ssao = new SSAO2RenderingPipeline("ssao", this.scene, { ssaoRatio: 0.5, blurRatio: 0.5 }, [this.camera]);
      ssao.radius = 1.4;
      ssao.totalStrength = 1.15;
      ssao.base = 0.12;
      ssao.samples = 12;
      ssao.maxZ = 60;
      ssao.minZAspect = 0.4;
      ssao.expensiveBlur = false;
      this.ssao = ssao;
      this.recompileFrozenMaterials();
    } else if (!tier.ssao && this.ssao) {
      this.ssao.dispose();
      this.ssao = null;
    }
  }

  // SSAO switches on the scene's prepass renderer, which adds depth/normal
  // outputs to every material's shader. Frozen materials skip define
  // re-evaluation, so any compiled before SSAO existed (Game freezes them all
  // at startup) would never write the G-buffer and the AO would sample
  // nothing. Thaw them, let the shaders recompile, then freeze them again.
  private recompileFrozenMaterials(): void {
    const frozen = this.scene.materials.filter((material) => material.isFrozen);
    if (frozen.length === 0) return;
    for (const material of frozen) material.unfreeze();
    this.scene.executeWhenReady(() => {
      if (this.scene.isDisposed) return;
      for (const material of frozen) material.freeze();
    });
  }

  public dispose(): void {
    this.ssao?.dispose();
    this.pipeline.dispose();
  }
}
