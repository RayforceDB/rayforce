# :material-speedometer: Performance Benchmark

Our benchmarks are based on the [H2OAI Group By Benchmark](https://h2oai.github.io/db-benchmark) standard.

<script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>

<style>
tr:has(td:first-child:contains("Rayforce")),
table tbody tr:nth-child(5) {
  background-color: rgba(233, 160, 51, 0.15) !important;
}
.performance-cards {
  display: flex;
  flex-wrap: wrap;
  gap: 1.5em;
  margin: 2em 0;
  justify-content: center;
}
.performance-card {
  flex: 1;
  min-width: 200px;
  max-width: 280px;
  background: var(--md-default-bg-color, #ffffff);
  border: 1px solid var(--bg-tertiary, #e0e0e0);
  border-radius: 8px;
  padding: 1.5em;
  text-align: center;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
  transition: all 0.2s ease;
}
[data-md-color-scheme="slate"] .performance-card {
  background: rgba(10, 58, 92, 0.2);
  border-color: rgba(233, 160, 51, 0.2);
}
.performance-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
  border-color: var(--accent-primary, #e9a033);
}
.performance-card .metric {
  font-size: 3em;
  font-weight: 700;
  color: var(--accent-primary, #e9a033);
  margin: 0.3em 0;
  line-height: 1;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
}
.performance-card .description {
  font-size: 1em;
  color: var(--text-secondary, #757575);
  margin-top: 0.5em;
  line-height: 1.4;
  font-weight: 400;
}
.performance-card .description:last-child {
  font-size: 0.75em;
  margin-top: 0.3em;
  opacity: 0.8;
}
[data-md-color-scheme="slate"] .performance-card .description {
  color: rgba(255, 255, 255, 0.7);
}
.performance-card-link {
  text-decoration: none;
  color: inherit;
  display: block;
}
.performance-card-link:hover {
  text-decoration: none;
}
</style>

<script>
document.addEventListener('DOMContentLoaded', function() {
  document.querySelectorAll('table tbody tr').forEach(function(row) {
    if (row.cells[0] && row.cells[0].textContent.trim() === 'Rayforce') {
      row.style.backgroundColor = 'rgba(233, 160, 51, 0.15)';
      row.style.fontWeight = '500';
    }
  });
});
</script>

<div class="performance-cards">

<a href="./window-join.html" class="performance-card-link">
<div class="performance-card">
<div class="metric">33.5x</div>
<div class="description">Faster Window Join</div>
<div class="description">(in comparison to ? 4.0)</div>
</div>
</a>

<a href="./inner-join.html" class="performance-card-link">
<div class="performance-card">
<div class="metric">11.6x</div>
<div class="description">Faster Inner Join</div>
<div class="description">(on average)</div>
</div>
</a>

<a href="./group-by.html" class="performance-card-link">
<div class="performance-card">
<div class="metric">3.7x</div>
<div class="description">Faster Group By</div>
<div class="description">(on average)</div>
</div>
</a>

</div>

[:material-group: Group By](./group-by.md) - demonstrates competitive performance across all 7 group by queries, consistently ranking among the top performers.

[:material-set-left-right: Left Join](./left-join.md) - excels in left join operations, successfully completing queries where DuckDB and ClickHouse encounter Out of Memory errors.

[:material-border-inside: Inner Join](./inner-join.md) - demonstrates exceptional performance in inner join operations, delivering the fastest execution time among all systems.

[:material-window-restore: Window Join](./window-join.md) - delivers dramatic performance improvements in window join operations, reducing execution time from over half an hour to under one minute.
