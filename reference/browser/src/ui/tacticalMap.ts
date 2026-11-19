import { PlayerController } from "../player/PlayerController";

// Draws all collision obstacles as rotated rectangles onto a top-down tactical
// canvas. Caller sets `ctx.fillStyle` beforehand.
export function paintObstacles(
  ctx: CanvasRenderingContext2D,
  scale: number,
  toX: (wx: number) => number,
  toY: (wz: number) => number
): void {
  for (const obs of PlayerController.getObstacles()) {
    ctx.save();
    ctx.translate(toX(obs.cx), toY(obs.cz));
    ctx.rotate(obs.yaw);
    ctx.fillRect(-obs.hw * scale, -obs.hd * scale, obs.hw * 2 * scale, obs.hd * 2 * scale);
    ctx.restore();
  }
}

// Traces a symmetric 4-point heading wedge centered at the origin and pointing
// up (-Y). Caller sets fillStyle/shadow and calls ctx.fill().
export function drawHeadingWedge(
  ctx: CanvasRenderingContext2D,
  halfW: number,
  tipY: number,
  notchY: number,
  wingY: number
): void {
  ctx.beginPath();
  ctx.moveTo(0, tipY);
  ctx.lineTo(halfW, wingY);
  ctx.lineTo(0, notchY);
  ctx.lineTo(-halfW, wingY);
  ctx.closePath();
}
