"use client";

import { fmtNumber } from "@/lib/format";
import { OrbitControls } from "@react-three/drei";
import { Canvas, useFrame, useThree } from "@react-three/fiber";
import { useLayoutEffect, useMemo, useRef, type RefObject } from "react";
import * as THREE from "three";

/** One sample of the cube. Axes match `ST_X`, `ST_Y` and `ST_Z`, with Y up. */
export interface CloudPoint {
  x: number;
  y: number;
  z: number;
}

/** One instanced sphere per sample. The radius tracks the cube so the balls stay small. */
function Cloud({ points, extent }: { points: CloudPoint[]; extent: number }) {
  const mesh = useRef<THREE.InstancedMesh>(null);
  const radius = Math.max(Math.abs(extent) || 1, 1) * 0.016;
  useLayoutEffect(() => {
    const current = mesh.current;
    if (!current) return;
    const dummy = new THREE.Object3D();
    for (let i = 0; i < points.length; i++) {
      dummy.position.set(points[i].x, points[i].y, points[i].z);
      dummy.updateMatrix();
      current.setMatrixAt(i, dummy.matrix);
    }
    current.instanceMatrix.needsUpdate = true;
    current.computeBoundingSphere();
  }, [points]);
  return (
    <instancedMesh key={points.length} ref={mesh} args={[undefined, undefined, points.length]}>
      <sphereGeometry args={[radius, 16, 12]} />
      <meshStandardMaterial color="#0284c7" roughness={0.45} metalness={0} />
    </instancedMesh>
  );
}

/** Writes the camera's spherical coordinates into the readout, every frame. */
function Readout({
  radius,
  theta,
  phi,
}: {
  radius: RefObject<HTMLSpanElement | null>;
  theta: RefObject<HTMLSpanElement | null>;
  phi: RefObject<HTMLSpanElement | null>;
}) {
  const camera = useThree((state) => state.camera);
  const spherical = useMemo(() => new THREE.Spherical(), []);
  useFrame(() => {
    spherical.setFromVector3(camera.position);
    if (radius.current) radius.current.textContent = fmtNumber(spherical.radius, 1);
    if (theta.current) theta.current.textContent = fmtNumber(THREE.MathUtils.radToDeg(spherical.theta), 1);
    if (phi.current) phi.current.textContent = fmtNumber(THREE.MathUtils.radToDeg(spherical.phi), 1);
  });
  return null;
}

/**
 * The history window as instanced spheres. Dragging orbits the camera on a
 * sphere around the origin; the wheel changes the radius. `r`, `θ` and `φ`
 * are that sphere: radius, azimuth around Y, and polar angle from +Y.
 */
export function PointCloud({
  points,
  extent = 100,
  testId = "point-cloud",
}: {
  points: CloudPoint[];
  extent?: number;
  testId?: string;
}) {
  const radius = useRef<HTMLSpanElement>(null);
  const theta = useRef<HTMLSpanElement>(null);
  const phi = useRef<HTMLSpanElement>(null);
  if (points.length === 0) {
    return (
      <div
        className="flex h-40 items-center justify-center rounded-lg bg-zinc-50 text-sm text-zinc-500 ring-1 ring-zinc-950/5"
        data-testid={testId}
        data-points={0}
      >
        Esperando muestras…
      </div>
    );
  }
  const distance = Math.max(Math.abs(extent) || 1, 1) * 2.2;
  const eye = new THREE.Vector3(distance * 0.7, distance * 0.5, distance * 0.7);
  const pose = new THREE.Spherical().setFromVector3(eye);
  return (
    <div data-testid={testId} data-points={points.length}>
      <p className="mb-2 text-sm tabular-nums text-zinc-500">
        r <span ref={radius} data-testid="spherical-r">{fmtNumber(pose.radius, 1)}</span>
        {" · θ "}
        <span ref={theta} data-testid="spherical-theta">{fmtNumber(THREE.MathUtils.radToDeg(pose.theta), 1)}</span>
        {"° · φ "}
        <span ref={phi} data-testid="spherical-phi">{fmtNumber(THREE.MathUtils.radToDeg(pose.phi), 1)}</span>
        °
      </p>
      <div className="h-[32rem] w-full max-w-xl overflow-hidden rounded-lg bg-zinc-50 ring-1 ring-zinc-950/5">
        <Canvas camera={{ position: [eye.x, eye.y, eye.z], fov: 45, near: 0.1, far: distance * 20 }}>
          <color attach="background" args={["#fafafa"]} />
          <ambientLight intensity={0.7} />
          <directionalLight position={[80, 140, 60]} intensity={1.15} />
          <Cloud points={points} extent={extent} />
          <axesHelper args={[Math.max(Math.abs(extent) || 1, 1)]} />
          <OrbitControls makeDefault enableDamping target={[0, 0, 0]} />
          <Readout radius={radius} theta={theta} phi={phi} />
        </Canvas>
      </div>
    </div>
  );
}
