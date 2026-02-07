---
hide:
  - navigation
  - toc
---

<div class="hero-section" markdown>
<div class="hero-content" markdown>

<div class="scroll-fade-in">
<img src="images/logo_dark_full.svg" alt="RayforceDB" class="hero-logo light-only">
<img src="images/logo_light_full.svg" alt="RayforceDB" class="hero-logo dark-only">
</div>

# Your data is **about to get faster** {.scroll-fade-in}

<div class="hero-description scroll-fade-in">
  RayforceDB is a high-performance columnar vector database written in pure C. Combines columnar storage with SIMD vectorization for lightning-fast analytics on time-series and big data workloads.
</div>


<div class="hero-buttons scroll-fade-in">
<a href="content/get-started/overview.html" class="md-button md-button--primary">Get Started
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
<line x1="5" y1="12" x2="19" y2="12"></line>
<polyline points="12 5 19 12 12 19"></polyline>
</svg>
</a>
<a href="https://wasm.rayforcedb.com/" class="md-button" target="_blank">Try It Now</a>
</div>

<div class="hero-socials scroll-fade-in" markdown>
[:fontawesome-brands-github:](https://github.com/singaraiona/rayforce "GitHub")
[:fontawesome-brands-x-twitter:](https://x.com/RayforceDB "X (Twitter)")
[:fontawesome-brands-reddit:](https://www.reddit.com/r/rayforcedb/ "Reddit")
[:simple-zulip:](https://rayforcedb.zulipchat.com/join/l33sichu4vp7nf77hgdul4om/ "Zulip Chat")
</div>

</div>

<div class="hero-ribbon">
<div class="ribbon-particles"></div>
<div class="ribbon-wave"></div>
<div class="ribbon-track">
<div class="ribbon-content">
<span class="ribbon-item"><span class="ribbon-icon">&#9889;</span> Sub-ms Queries</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9638;</span> Columnar Storage</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128230;</span> Under 1MB</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128295;</span> Zero Dependencies</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9729;</span> Cloud Native</span>
<span class="ribbon-item"><span class="ribbon-icon">&#123;&#125;</span> In-Process</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9654;</span> SIMD Vectorized</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128274;</span> MIT License</span>
</div>
<div class="ribbon-content" aria-hidden="true">
<span class="ribbon-item"><span class="ribbon-icon">&#9889;</span> Sub-ms Queries</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9638;</span> Columnar Storage</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128230;</span> Under 1MB</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128295;</span> Zero Dependencies</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9729;</span> Cloud Native</span>
<span class="ribbon-item"><span class="ribbon-icon">&#123;&#125;</span> In-Process</span>
<span class="ribbon-item"><span class="ribbon-icon">&#9654;</span> SIMD Vectorized</span>
<span class="ribbon-item"><span class="ribbon-icon">&#128274;</span> MIT License</span>
</div>
</div>
</div>
</div>

<div class="features-section" markdown>

## Think fast. **Build faster.** {.scroll-fade-in}

<div class="grid cards scroll-fade-in" markdown>

- :material-lightning-bolt:{ .lg .middle .feature-icon } **Blazing Fast**

    ---

    Columnar storage with vectorized operations delivers sub-millisecond query performance on analytical workloads.

- :fontawesome-solid-feather-pointed:{ .lg .middle .feature-icon } **Simple Syntax**

    ---

    Intuitive LISP-like syntax flattens the learning curve. Write powerful queries in minutes, not hours.

    [:octicons-arrow-right-24: Learn Syntax](content/documentation/overview.md)

- :material-language-c:{ .lg .middle .feature-icon } **Pure C, Zero Dependencies**

    ---

    Under 1MB binary fits in CPU cache. No external dependencies means simple deployment anywhere.

- :material-rocket-launch:{ .lg .middle .feature-icon } **Production Ready**

    ---

    Battle-tested columnar engine optimized for real-world analytics and data processing pipelines.

- :material-memory:{ .lg .middle .feature-icon } **In-Process Embedding**

    ---

    No separate database server needed. Embed directly in your application for immediate data access with zero network overhead.

- :material-cloud-outline:{ .lg .middle .feature-icon } **Cloud Native Scaling**

    ---

    Scale effortlessly on any cloud. No limits, no bottlenecks. Save costs by optimizing memory usage and CPU hours.

</div>

</div>

<div class="code-demo-section">

<div class="code-demo-header scroll-fade-in">
<h2>Expressive. <strong>Readable.</strong> Fast.</h2>
<p>Rayfall combines the power of array programming with the clarity of Lisp syntax.</p>
</div>

<div class="code-tabs scroll-fade-in">
<div class="code-tab-list">
<button class="code-tab active" data-tab="select">Select</button>
<button class="code-tab" data-tab="insert">Insert</button>
<button class="code-tab" data-tab="update">Update</button>
<button class="code-tab" data-tab="upsert">Upsert</button>
<button class="code-tab" data-tab="alter">Alter</button>
<button class="code-tab" data-tab="joins">Joins</button>
</div>

<div class="code-panels">
<div class="code-panel active" data-panel="select">
<div class="code-block">
<pre><code><span class="code-comment">; Create a table</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>name dept salary hire_date<span class="code-paren">]</span> 
  <span class="code-paren">(</span><span class="code-func">list</span> 
    <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-string">"Alice"</span> <span class="code-string">"Bob"</span> <span class="code-string">"Charlie"</span> <span class="code-string">"David"</span><span class="code-paren">)</span> 
    <span class="code-paren">[</span><span class="code-symbol">'IT</span> <span class="code-symbol">'HR</span> <span class="code-symbol">'IT</span> <span class="code-symbol">'IT</span><span class="code-paren">]</span> 
    <span class="code-paren">[</span><span class="code-number">75000</span> <span class="code-number">65000</span> <span class="code-number">85000</span> <span class="code-number">72000</span><span class="code-paren">]</span> 
    <span class="code-paren">[</span><span class="code-number">2021.01.15</span> <span class="code-number">2020.03.20</span> <span class="code-number">2019.11.30</span> <span class="code-number">2022.05.10</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Select with filtering</span>
<span class="code-paren">(</span><span class="code-keyword">select</span> <span class="code-paren">{</span>
  name: name 
  salary: salary 
  from: employees 
  where: <span class="code-paren">(</span><span class="code-func">&gt;</span> salary <span class="code-number">70000</span><span class="code-paren">)</span><span class="code-paren">}</span><span class="code-paren">)</span>

<span class="code-comment">; Group by and aggregate</span>
<span class="code-paren">(</span><span class="code-keyword">select</span> <span class="code-paren">{</span>
  avg_salary: <span class="code-paren">(</span><span class="code-func">avg</span> salary<span class="code-paren">)</span>
  headcount: <span class="code-paren">(</span><span class="code-func">count</span> name<span class="code-paren">)</span>
  from: employees
  by: dept<span class="code-paren">}</span><span class="code-paren">)</span></code></pre>
</div>
</div>

<div class="code-panel" data-panel="insert">
<div class="code-block">
<pre><code><span class="code-comment">; Create a table</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>name age<span class="code-paren">]</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-symbol">'Alice</span> <span class="code-symbol">'Bob</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">25</span> <span class="code-number">30</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Insert a single row</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">insert</span> employees <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-symbol">'Charlie</span> <span class="code-number">35</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Insert multiple rows</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">insert</span> employees <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-symbol">'David</span> <span class="code-symbol">'Eve</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">40</span> <span class="code-number">25</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; In-place insertion</span>
<span class="code-paren">(</span><span class="code-func">insert</span> <span class="code-symbol">'employees</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-string">"Mike"</span> <span class="code-number">75</span><span class="code-paren">)</span><span class="code-paren">)</span></code></pre>
</div>
</div>

<div class="code-panel" data-panel="update">
<div class="code-block">
<pre><code><span class="code-comment">; Create a table</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>name dept salary<span class="code-paren">]</span> 
  <span class="code-paren">(</span><span class="code-func">list</span> 
    <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-string">"Alice"</span> <span class="code-string">"Bob"</span> <span class="code-string">"Charlie"</span><span class="code-paren">)</span> 
    <span class="code-paren">[</span><span class="code-symbol">'IT</span> <span class="code-symbol">'HR</span> <span class="code-symbol">'IT</span><span class="code-paren">]</span> 
    <span class="code-paren">[</span><span class="code-number">75000</span> <span class="code-number">65000</span> <span class="code-number">85000</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Update with where clause</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">update</span> <span class="code-paren">{</span>
  salary: <span class="code-paren">(</span><span class="code-func">*</span> salary <span class="code-number">1.1</span><span class="code-paren">)</span>
  from: employees
  where: <span class="code-paren">(</span><span class="code-func">&gt;</span> salary <span class="code-number">70000</span><span class="code-paren">)</span><span class="code-paren">}</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Update with grouping</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">update</span> <span class="code-paren">{</span>
  salary: <span class="code-paren">(</span><span class="code-func">+</span> salary <span class="code-number">1000</span><span class="code-paren">)</span>
  from: employees
  by: dept
  where: <span class="code-paren">(</span><span class="code-func">&gt;</span> salary <span class="code-number">55000</span><span class="code-paren">)</span><span class="code-paren">}</span><span class="code-paren">)</span><span class="code-paren">)</span></code></pre>
</div>
</div>

<div class="code-panel" data-panel="upsert">
<div class="code-block">
<pre><code><span class="code-comment">; Create a table with key column</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>id name age<span class="code-paren">]</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-number">1</span> <span class="code-number">2</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-symbol">'Alice</span> <span class="code-symbol">'Bob</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">25</span> <span class="code-number">30</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Upsert: updates id=2, inserts id=3</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">upsert</span> employees <span class="code-number">1</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-number">2</span> <span class="code-number">3</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-symbol">'Bob-updated</span> <span class="code-symbol">'Charlie</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">30</span> <span class="code-number">35</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Upsert single row</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> employees <span class="code-paren">(</span><span class="code-func">upsert</span> employees <span class="code-number">1</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-number">4</span> <span class="code-symbol">'David</span> <span class="code-number">40</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; In-place upsert</span>
<span class="code-paren">(</span><span class="code-func">upsert</span> <span class="code-symbol">'employees</span> <span class="code-number">1</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-number">11</span> <span class="code-symbol">'Kate</span> <span class="code-number">27</span><span class="code-paren">)</span><span class="code-paren">)</span></code></pre>
</div>
</div>

<div class="code-panel" data-panel="alter">
<div class="code-block">
<pre><code><span class="code-comment">; Alter vector elements</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> prices <span class="code-paren">[</span><span class="code-number">100</span> <span class="code-number">200</span> <span class="code-number">300</span><span class="code-paren">]</span><span class="code-paren">)</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> prices <span class="code-paren">(</span><span class="code-func">alter</span> prices <span class="code-func">+</span> <span class="code-number">1</span> <span class="code-number">10</span><span class="code-paren">)</span><span class="code-paren">)</span>
<span class="code-result">; → [100 210 300]</span>

<span class="code-comment">; Alter table column</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> trades <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>price volume<span class="code-paren">]</span> <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-number">100</span> <span class="code-number">200</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">50</span> <span class="code-number">60</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> trades <span class="code-paren">(</span><span class="code-func">alter</span> trades <span class="code-func">+</span> <span class="code-symbol">'price</span> <span class="code-number">10</span><span class="code-paren">)</span><span class="code-paren">)</span>
<span class="code-result">; → price: [110 210], volume: [50 60]</span>

<span class="code-comment">; In-place alteration</span>
<span class="code-paren">(</span><span class="code-func">alter</span> <span class="code-symbol">'prices</span> <span class="code-func">+</span> <span class="code-number">10</span><span class="code-paren">)</span></code></pre>
</div>
</div>

<div class="code-panel" data-panel="joins">
<div class="code-block">
<pre><code><span class="code-comment">; Create tables</span>
<span class="code-paren">(</span><span class="code-keyword">set</span> trades <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>symbol order_id price quantity<span class="code-paren">]</span> 
    <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-symbol">'AAPL</span> <span class="code-symbol">'MSFT</span> <span class="code-symbol">'GOOG</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">1001</span> <span class="code-number">1002</span> <span class="code-number">1003</span><span class="code-paren">]</span> 
          <span class="code-paren">[</span><span class="code-number">150.25</span> <span class="code-number">300.50</span> <span class="code-number">125.75</span><span class="code-paren">]</span> <span class="code-paren">[</span><span class="code-number">100</span> <span class="code-number">200</span> <span class="code-number">150</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-paren">(</span><span class="code-keyword">set</span> orders <span class="code-paren">(</span><span class="code-func">table</span> <span class="code-paren">[</span>order_id client_id timestamp status<span class="code-paren">]</span> 
   <span class="code-paren">(</span><span class="code-func">list</span> <span class="code-paren">[</span><span class="code-number">1001</span> <span class="code-number">1002</span> <span class="code-number">1004</span><span class="code-paren">]</span> 
         <span class="code-paren">[</span><span class="code-symbol">'CLIENT_A</span> <span class="code-symbol">'CLIENT_B</span> <span class="code-symbol">'CLIENT_C</span><span class="code-paren">]</span> 
         <span class="code-paren">[</span><span class="code-number">09:00:00</span> <span class="code-number">09:05:00</span> <span class="code-number">09:10:00</span><span class="code-paren">]</span> 
         <span class="code-paren">[</span><span class="code-symbol">'FILLED</span> <span class="code-symbol">'FILLED</span> <span class="code-symbol">'PENDING</span><span class="code-paren">]</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span><span class="code-paren">)</span>

<span class="code-comment">; Left join - keeps all rows from left table</span>
<span class="code-paren">(</span><span class="code-func">left-join</span> <span class="code-paren">[</span>order_id<span class="code-paren">]</span> trades orders<span class="code-paren">)</span>

<span class="code-comment">; Inner join - only matching rows</span>
<span class="code-paren">(</span><span class="code-func">inner-join</span> <span class="code-paren">[</span>order_id<span class="code-paren">]</span> trades orders<span class="code-paren">)</span>

<span class="code-comment">; As-of join for time-series data</span>
<span class="code-paren">(</span><span class="code-func">asof-join</span> <span class="code-paren">[</span>Sym Ts<span class="code-paren">]</span> trades quotes<span class="code-paren">)</span></code></pre>
</div>
</div>
</div>
</div>

<script>
document.addEventListener('DOMContentLoaded', function() {
  // Tab switching functionality
  document.querySelectorAll('.code-tab').forEach(tab => {
    tab.addEventListener('click', function() {
      // Remove active class from all tabs and panels
      document.querySelectorAll('.code-tab').forEach(t => t.classList.remove('active'));
      document.querySelectorAll('.code-panel').forEach(p => p.classList.remove('active'));
      
      // Add active class to clicked tab
      this.classList.add('active');
      
      // Show corresponding panel
      const tabName = this.dataset.tab;
      const panel = document.querySelector(`.code-panel[data-panel="${tabName}"]`);
      if (panel) {
        panel.classList.add('active');
      }
    });
  });

  // Copy button functionality
  document.querySelectorAll('.code-copy').forEach(button => {
    button.addEventListener('click', function() {
      const panel = this.closest('.code-panel');
      const codeBlock = panel.querySelector('code');
      const code = codeBlock.textContent;
      
      navigator.clipboard.writeText(code).then(() => {
        const originalText = this.textContent;
        this.textContent = 'Copied!';
        setTimeout(() => {
          this.textContent = originalText;
        }, 2000);
      });
    });
  });
});
</script>

<style>
/* Scroll fade-in animation styles */
.scroll-fade-in {
  opacity: 0;
  transform: translateY(30px);
  transition: opacity 0.8s ease-out, transform 0.8s ease-out;
}

.scroll-fade-in.visible {
  opacity: 1;
  transform: translateY(0);
}

/* Logo container and ticker - visible by default, animate on load */
.hero-content > .scroll-fade-in:first-child,
.hero-ticker.scroll-fade-in {
  opacity: 1 !important;
  transform: translateY(0) !important;
  transition-delay: 0s !important;
}

.hero-content > .scroll-fade-in:first-child img {
  opacity: 1 !important;
}

/* Stagger delays for hero section elements */
.hero-content > h1.scroll-fade-in,
.hero-content > .hero-description.scroll-fade-in {
  transition-delay: 0.2s;
}

.hero-content > .hero-buttons.scroll-fade-in {
  transition-delay: 0.3s;
}

.hero-content > .hero-socials.scroll-fade-in {
  transition-delay: 0.4s;
}
</style>

<script>
document.addEventListener('DOMContentLoaded', function() {
  // Intersection Observer for scroll animations
  const observerOptions = {
    threshold: 0.1,
    rootMargin: '0px 0px -50px 0px'
  };

  const observer = new IntersectionObserver(function(entries) {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        entry.target.classList.add('visible');
        // Optional: unobserve after animation to improve performance
        // observer.unobserve(entry.target);
      }
    });
  }, observerOptions);

  // Observe all elements with scroll-fade-in class except logo and ticker
  document.querySelectorAll('.scroll-fade-in').forEach(el => {
    // Skip the logo container and ticker - they're always visible
    if (!el.matches('.hero-content > .scroll-fade-in:first-child') && 
        !el.matches('.hero-ticker.scroll-fade-in')) {
      observer.observe(el);
    } else {
      // Logo and ticker are always visible, mark them as visible
      el.classList.add('visible');
    }
  });
});
</script>
</div>


<div id="ecosystem" class="features-section" markdown>

## Integrate with your **Stack** {.scroll-fade-in}

<div class="grid cards" markdown>

- :fontawesome-brands-python:{ .lg .middle .feature-icon } **Python Bindings**

    ---

    Native Python integration through FFI bindings. Use RayforceDB directly from your Python applications with zero overhead.

    [:octicons-arrow-right-24: Python Docs](https://py.rayforcedb.com)

- :material-web:{ .lg .middle .feature-icon } **WebAssembly**

    ---

    Run RayforceDB directly in your browser. Try the interactive WASM playground with zero installation required.

    [:octicons-arrow-right-24: Try Playground](https://wasm.rayforcedb.com/)

- :material-language-c:{ .lg .middle .feature-icon } **C Plugin System**

    ---

    Extend functionality with dynamic C plugins. Load custom functions and modules at runtime for maximum flexibility.

    [:octicons-arrow-right-24: Load Function](content/documentation/REPL.md#load-function)

</div>

</div>


<div class="cta-section scroll-fade-in" markdown>

## Get started in **minutes**

RayforceDB is open source under MIT license. Download, build, and run your first query today.

[Read the Docs](content/documentation/overview.md){ .md-button .md-button--primary }

</div>
