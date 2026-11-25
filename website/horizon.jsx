import { createRoot } from 'react-dom/client';
import GradientASCII from './GradientASCII';

const host = document.getElementById('wavespan-horizon');
const root = createRoot(host);
const scene = host.closest('main').parentElement;
const brand = scene.querySelector('.header .brand');
const title = scene.querySelector('.hero-copy');
const motion = matchMedia('(prefers-reduced-motion: reduce)');
let entrance = [];

function finishEntrance() {
  entrance.forEach(animation => animation.cancel());
  entrance = [];
}

function enter() {
  finishEntrance();
  if (motion.matches || !brand.animate) return;
  const start = title.getBoundingClientRect();
  const end = brand.getBoundingClientRect();
  const offsetX = start.left - end.left;
  const offsetY = start.top + Math.min(start.height / 2, 150) - end.top - end.height / 2;
  const scale = Math.min(2.65, start.width / end.width);
  const pose = `translate(${offsetX}px, ${offsetY}px) scale(${scale})`;
  entrance.push(brand.animate([
    { transform: pose, opacity: 0 },
    { transform: pose, opacity: 1, offset: 0.22 },
    { transform: pose, opacity: 1, offset: 0.38, easing: 'cubic-bezier(.45,0,.15,1)' },
    { transform: 'none', opacity: 1 }
  ], 1750));
  entrance.push(title.animate([
    { opacity: 0, transform: 'translateY(9px)' },
    { opacity: 1, transform: 'none' }
  ], { delay: 930, duration: 850, easing: 'ease-out', fill: 'backwards' }));
}

function showHorizon() {
  root.render(
    <GradientASCII
      colors={['#1e3fd8', '#f6f8ff', '#a5d8ff', '#38bdf8']}
      shape="horizon"
      characters=" =+xX0S8#@"
      cellSize={18}
      density={0.8}
      glyphOpacity={0.85}
      glow={0.2}
      depthOfField={0}
      flicker={0.2}
      softness={0.5}
      scale={1}
      flow={0.5}
      speed={1}
      gradientOpacity={0.9}
      grain={0.08}
      variation={0}
      mouseInteraction
      scrambleRadius={0.1}
      clickEffect="wave"
      intro
      lightMode={false}
    />
  );
}

scene.addEventListener('focusin', finishEntrance);
motion.addEventListener('change', finishEntrance);
host.addEventListener('wavespan-replay', enter);
showHorizon();
enter();

