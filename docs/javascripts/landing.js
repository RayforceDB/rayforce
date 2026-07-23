document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  const nav = document.querySelector('.nav');

  const sections = Array.from(document.querySelectorAll('section[id]'));
  const sectionLinks = Array.from(document.querySelectorAll('.nav-links a[href*="#"]'));
  const updateNav = () => {
    if (nav) nav.classList.toggle('nav-scrolled', window.scrollY > 32);
    const current = sections.findLast
      ? sections.findLast((section) => section.offsetTop <= window.scrollY + 180)
      : sections.slice().reverse().find((section) => section.offsetTop <= window.scrollY + 180);
    sectionLinks.forEach((link) => {
      link.classList.toggle('active', Boolean(current && link.hash === '#' + current.id));
    });
  };
  window.addEventListener('scroll', updateNav, { passive: true });
  updateNav();

  const reduceMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const reveals = document.querySelectorAll('.reveal');
  if (reduceMotion || !('IntersectionObserver' in window)) {
    reveals.forEach((el) => el.classList.add('is-visible'));
  } else {
    const revealObserver = new IntersectionObserver((entries) => {
      entries.forEach((entry) => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add('is-visible');
        revealObserver.unobserve(entry.target);
      });
    }, { threshold: 0.14, rootMargin: '0px 0px -6% 0px' });
    reveals.forEach((el) => revealObserver.observe(el));
  }

  document.querySelectorAll('[data-code-tabs]').forEach((tabs) => {
    const buttons = tabs.querySelectorAll('[data-code-tab]');
    const panels = tabs.querySelectorAll('[data-code-panel]');
    buttons.forEach((button) => {
      button.addEventListener('click', () => {
        const selected = button.dataset.codeTab;
        buttons.forEach((item) => item.setAttribute('aria-selected', String(item === button)));
        panels.forEach((panel) => { panel.hidden = panel.dataset.codePanel !== selected; });
      });
    });
  });

  document.querySelectorAll('[data-live-demo]').forEach((demo) => {
    const label = demo.querySelector('[data-demo-label]');
    const progress = demo.querySelector('[data-demo-progress]');
    const progressTrack = demo.querySelector('[data-demo-progress-track]');
    const result = demo.querySelector('[data-demo-result]');
    const replay = demo.querySelector('[data-demo-replay]');
    const stages = Array.from(demo.querySelectorAll('[data-demo-stage]'));
    const frames = [
      { stage: 'load', label: 'Loading 100,000 rows', progress: 16, delay: 1050 },
      { stage: 'optimize', label: 'Rewriting the lazy DAG', progress: 40, delay: 1250 },
      { stage: 'filter', label: 'Filtering qty > 100 · 60,000 match', progress: 68, delay: 1200 },
      { stage: 'group', label: 'Grouping in parallel by symbol', progress: 90, delay: 1150 },
      { stage: 'result', label: 'Complete · 100,000 rows → 4 groups', progress: 100, delay: 3800 }
    ];
    let frameIndex = 0;
    let timer = 0;
    let visible = true;

    const paint = (frame) => {
      label.textContent = frame.label;
      progress.style.width = frame.progress + '%';
      progressTrack.setAttribute('aria-valuenow', String(frame.progress));
      stages.forEach((stage, index) => {
        const activeIndex = Math.min(frameIndex, stages.length - 1);
        stage.classList.toggle('is-active', frame.stage !== 'result' && index === activeIndex);
        stage.classList.toggle('is-complete', frame.stage === 'result' || index < activeIndex);
      });
      result.classList.toggle('is-visible', frame.stage === 'result');
    };

    const advance = () => {
      if (!visible) return;
      paint(frames[frameIndex]);
      timer = window.setTimeout(() => {
        frameIndex = (frameIndex + 1) % frames.length;
        advance();
      }, frames[frameIndex].delay);
    };

    const restart = () => {
      window.clearTimeout(timer);
      frameIndex = 0;
      result.classList.remove('is-visible');
      advance();
    };

    replay.addEventListener('click', restart);

    if (reduceMotion) {
      frameIndex = frames.length - 1;
      paint(frames[frameIndex]);
      return;
    }

    if ('IntersectionObserver' in window) {
      const demoObserver = new IntersectionObserver(([entry]) => {
        visible = entry.isIntersecting;
        window.clearTimeout(timer);
        if (visible) advance();
      }, { threshold: 0.15 });
      visible = false;
      demoObserver.observe(demo);
    } else {
      advance();
    }
  });
});

(function loadGitHubStats() {
  const targets = document.querySelectorAll('[data-gh-stat]');
  if (!targets.length) return;

  const repo = 'RayforceDB/rayforce';
  const cacheKey = 'rayforce:github-stats';
  const format = (value) => {
    if (typeof value !== 'number') return '—';
    return value >= 1000 ? (value / 1000).toFixed(value >= 10000 ? 0 : 1) + 'k' : String(value);
  };
  const paint = (data) => targets.forEach((el) => {
    el.textContent = format(el.dataset.ghStat === 'forks' ? data.forks : data.stars);
  });

  try {
    const cached = JSON.parse(localStorage.getItem(cacheKey));
    if (cached && Date.now() - cached.savedAt < 3600000) {
      paint(cached);
      return;
    }
  } catch (_) {}

  fetch('https://ungh.cc/repos/' + repo)
    .then((response) => response.ok ? response.json() : Promise.reject(new Error('GitHub stats unavailable')))
    .then((data) => {
      const stats = { stars: data.repo.stars, forks: data.repo.forks, savedAt: Date.now() };
      paint(stats);
      try { localStorage.setItem(cacheKey, JSON.stringify(stats)); } catch (_) {}
    })
    .catch(() => {});
})();
