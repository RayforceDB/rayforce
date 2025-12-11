# :material-database: Welcome to RayforceDB!

The <b>ultra-fast</b> columnar database engineered to deliver the <b>best high-speed performance</b> for your real-time storage solutions.

<div style="margin: 1.5rem 0;">
  <div style="display: flex; align-items: center; gap: 1rem; margin-bottom: 1rem;">
    <div style="flex: 1;">
      <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
        <span style="font-weight: 600;">RayforceDB</span>
        <span style="color: var(--accent-primary); font-weight: 700;">277 ms</span>
      </div>
      <div style="background: var(--bg-tertiary); height: 8px; border-radius: 4px; overflow: hidden;">
        <div style="background: linear-gradient(90deg, var(--accent-primary), var(--accent-hover)); height: 100%; width: 32.4%;"></div>
      </div>
    </div>
    <div style="color: var(--accent-primary); font-weight: 700; min-width: 60px; text-align: right;">3.08x</div>
  </div>

  <div style="display: flex; align-items: center; gap: 1rem; margin-bottom: 1rem;">
    <div style="flex: 1;">
      <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
        <span style="font-weight: 600;">DuckDB (MT on)</span>
        <span style="color: var(--text-secondary); font-weight: 700;">304 ms</span>
      </div>
      <div style="background: var(--bg-tertiary); height: 8px; border-radius: 4px; overflow: hidden;">
        <div style="background: linear-gradient(90deg, var(--text-secondary), var(--text-secondary)); height: 100%; width: 35.6%;"></div>
      </div>
    </div>
    <div style="color: var(--text-secondary); font-weight: 700; min-width: 60px; text-align: right;">2.81x</div>
  </div>
  
  <div style="display: flex; align-items: center; gap: 1rem; margin-bottom: 1rem;">
    <div style="flex: 1;">
      <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
        <span style="font-weight: 600;">ClickHouse</span>
        <span style="color: var(--text-secondary); font-weight: 700;">395 ms</span>
      </div>
      <div style="background: var(--bg-tertiary); height: 8px; border-radius: 4px; overflow: hidden;">
        <div style="background: linear-gradient(90deg, var(--text-secondary), var(--text-secondary)); height: 100%; width: 46.2%;"></div>
      </div>
    </div>
    <div style="color: var(--text-secondary); font-weight: 700; min-width: 60px; text-align: right;">2.16x</div>
  </div>
  
  <div style="display: flex; align-items: center; gap: 1rem;">
    <div style="flex: 1;">
      <div style="display: flex; justify-content: space-between; margin-bottom: 0.5rem;">
        <span style="font-weight: 600;">DuckDB (MT off)</span>
        <span style="color: var(--text-secondary); font-weight: 700;">855 ms</span>
      </div>
      <div style="background: var(--bg-tertiary); height: 8px; border-radius: 4px; overflow: hidden;">
        <div style="background: linear-gradient(90deg, var(--text-secondary), var(--text-tertiary)); height: 100%; width: 100%;"></div>
      </div>
    </div>
    <div style="color: var(--text-secondary); font-weight: 700; min-width: 60px; text-align: right;">1.00x</div>
  </div>
</div>

!!! note ""
    RayforceDB delivers exceptional performance, competing with the fastest databases while maintaining simplicity and efficiency. Our benchmarks are based on the [H2OAI Group By Benchmark](https://h2oai.github.io/db-benchmark/) standard. [View full benchmarks →](./benchmarks/overview.md)


<div class="grid cards" markdown>

- :fontawesome-solid-feather-pointed:{ .lg .middle } __Simple LISP syntax__

    ---

    Querying tables <b>has never been</b> this easy to follow...
    
    [:octicons-arrow-right-24: Syntax](../documentation/overview.md)

- :material-poll:{ .lg .middle } __Columnar storage__

    ---

    Columnar storage allows RayforceDB to outperform the alternative solutions
    
    [:octicons-arrow-right-24: Technical Details](./technical-details.md)


- :material-language-c:{ .lg .middle } __Plain C__

    ---

    The application size is less than a single PNG file - <b>under 1 MB!</b>

    It can fit entirely into the CPU cache!

- :material-scale-balance:{ .lg .middle } __Open Source, MIT__

    ---

    RayforceDB is licensed under MIT and available on [GitHub](https://github.com/RayforceDB/rayforce)

    [:octicons-arrow-right-24: Contribute](./contribute.md)

</div>
