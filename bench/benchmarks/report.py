"""
Static HTML report generation for GitHub Pages.

Generates a beautiful, interactive benchmark comparison website
with RayforceDB brand styling matching the official website.
"""

import json
from datetime import datetime
from pathlib import Path
from typing import Any

from .runner import BenchmarkResults
from .stats import BenchmarkStatistics, compute_statistics


def generate_report(
    results: BenchmarkResults,
    output_dir: Path,
    report_name: str | None = None,
) -> Path:
    """Generate a static HTML report from benchmark results.
    
    Args:
        results: Raw results from BenchmarkRunner.
        output_dir: Directory to write report files.
        report_name: Optional custom name (default: index for GitHub Pages).
    
    Returns:
        Path to generated HTML file.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    stats = compute_statistics(results)
    
    # For GitHub Pages, always generate index.html
    html_filename = "index.html"
    json_filename = "data.json"
    
    # Generate HTML content
    html_content = _generate_html(results, stats)
    
    # Write HTML file
    html_path = output_dir / html_filename
    with open(html_path, "w") as f:
        f.write(html_content)
    
    # Write JSON data for programmatic access
    json_path = output_dir / json_filename
    with open(json_path, "w") as f:
        json.dump(_results_to_dict(results, stats), f, indent=2)
    
    return html_path


def _generate_html(results: BenchmarkResults, stats: BenchmarkStatistics) -> str:
    """Generate complete HTML report content."""
    
    # Build data for charts
    chart_data = _prepare_chart_data(results, stats)
    
    # Build performance cards
    perf_cards = _build_performance_cards(stats)
    
    # Build results table
    results_table = _build_results_table(stats)
    
    # Build task detail sections
    task_sections = _build_task_sections(results, stats)
    
    timestamp = datetime.now().strftime("%B %d, %Y at %H:%M")
    
    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RayforceDB Benchmarks</title>
    <meta name="description" content="Performance benchmarks comparing RayforceDB against other databases">
    
    <link rel="icon" type="image/svg+xml" href="images/favicon.svg">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Roboto:ital,wght@0,300;0,400;0,500;0,700;1,400&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
    
    <!-- ECharts -->
    <script src="https://cdn.jsdelivr.net/npm/echarts@5.4.3/dist/echarts.min.js"></script>
    
    <style>
        /* ===================================
           RayforceDB Benchmark Report Styles
           Matching official website exactly
           =================================== */
        
        :root {{
            /* Brand Colors - exact match from official website */
            --primary-navy: #04233b;
            --primary-navy-light: #0a3a5c;
            --primary-navy-dark: #021828;
            --accent-golden: #e9a033;
            --accent-golden-light: #f0b85a;
            --accent-golden-dark: #c98a2a;
            
            /* Light Theme */
            --bg-primary: #ffffff;
            --bg-secondary: #f8f9fa;
            --bg-tertiary: #e9ecef;
            --text-primary: #04233b;
            --text-secondary: #495057;
            --text-tertiary: #6c757d;
            --border-color: #dee2e6;
            --card-bg: #ffffff;
            --card-shadow: 0 4px 24px rgba(4, 35, 59, 0.08);
            --card-shadow-hover: 0 12px 40px rgba(4, 35, 59, 0.15);
            
            /* Sizing */
            --container-max: 1200px;
            --header-height: 72px;
            --border-radius: 12px;
            --border-radius-lg: 20px;
            
            /* Transitions */
            --transition-fast: 0.15s ease;
            --transition-normal: 0.25s ease;
            
            /* Status */
            --success: #4CAF50;
            --error: #f44336;
        }}
        
        [data-theme="dark"] {{
            --bg-primary: #04233b;
            --bg-secondary: #021828;
            --bg-tertiary: #0a3a5c;
            --text-primary: #ffffff;
            --text-secondary: rgba(255, 255, 255, 0.8);
            --text-tertiary: rgba(255, 255, 255, 0.6);
            --border-color: rgba(255, 255, 255, 0.1);
            --card-bg: #0a3a5c;
            --card-shadow: 0 4px 24px rgba(0, 0, 0, 0.3);
            --card-shadow-hover: 0 12px 40px rgba(0, 0, 0, 0.4);
        }}
        
        *, *::before, *::after {{
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }}
        
        html {{
            scroll-behavior: smooth;
            -webkit-font-smoothing: antialiased;
            -moz-osx-font-smoothing: grayscale;
        }}
        
        body {{
            font-family: 'Roboto', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
            background-color: var(--bg-primary);
            color: var(--text-primary);
            line-height: 1.6;
            transition: background-color var(--transition-normal), color var(--transition-normal);
        }}
        
        a {{
            color: inherit;
            text-decoration: none;
        }}
        
        img {{
            max-width: 100%;
            height: auto;
        }}
        
        /* ===================================
           Header - exact match from website
           =================================== */
        
        .header {{
            position: fixed;
            top: 0;
            left: 0;
            right: 0;
            height: var(--header-height);
            background: var(--bg-primary);
            border-bottom: 1px solid var(--border-color);
            z-index: 1000;
            transition: background-color var(--transition-normal), border-color var(--transition-normal);
        }}
        
        .header-container {{
            max-width: var(--container-max);
            margin: 0 auto;
            padding: 0 24px;
            height: 100%;
            display: flex;
            align-items: center;
            justify-content: space-between;
        }}
        
        .logo-link {{
            display: flex;
            align-items: center;
        }}
        
        .logo {{
            height: 32px;
            width: auto;
        }}
        
        .logo-light {{
            display: block;
        }}
        
        .logo-dark {{
            display: none;
        }}
        
        [data-theme="dark"] .logo-light {{
            display: none;
        }}
        
        [data-theme="dark"] .logo-dark {{
            display: block;
        }}
        
        .nav {{
            display: flex;
            align-items: center;
            gap: 32px;
        }}
        
        .nav-link {{
            font-weight: 500;
            font-size: 1rem;
            color: var(--text-secondary);
            transition: color var(--transition-fast);
        }}
        
        .nav-link:hover {{
            color: var(--accent-golden);
        }}
        
        .theme-toggle {{
            display: flex;
            align-items: center;
            justify-content: center;
            width: 40px;
            height: 40px;
            border: none;
            background: var(--bg-tertiary);
            border-radius: 10px;
            cursor: pointer;
            color: var(--text-secondary);
            transition: all var(--transition-fast);
        }}
        
        .theme-toggle:hover {{
            background: var(--accent-golden);
            color: var(--primary-navy);
        }}
        
        .theme-toggle svg {{
            width: 20px;
            height: 20px;
        }}
        
        .sun-icon {{
            display: none;
        }}
        
        .moon-icon {{
            display: block;
        }}
        
        [data-theme="dark"] .sun-icon {{
            display: block;
        }}
        
        [data-theme="dark"] .moon-icon {{
            display: none;
        }}
        
        /* ===================================
           Hero Section - matching website
           =================================== */
        
        .hero {{
            position: relative;
            padding: calc(var(--header-height) + 60px) 24px 80px;
            text-align: center;
            overflow: hidden;
        }}
        
        .hero-bg {{
            position: absolute;
            inset: 0;
            z-index: 0;
        }}
        
        .hero-gradient {{
            position: absolute;
            inset: 0;
            background: 
                radial-gradient(ellipse 80% 50% at 50% -20%, rgba(233, 160, 51, 0.15), transparent),
                radial-gradient(ellipse 60% 40% at 80% 80%, rgba(4, 35, 59, 0.1), transparent),
                radial-gradient(ellipse 40% 30% at 10% 60%, rgba(233, 160, 51, 0.08), transparent);
        }}
        
        [data-theme="dark"] .hero-gradient {{
            background: 
                radial-gradient(ellipse 80% 50% at 50% -20%, rgba(233, 160, 51, 0.2), transparent),
                radial-gradient(ellipse 60% 40% at 80% 80%, rgba(233, 160, 51, 0.1), transparent),
                radial-gradient(ellipse 40% 30% at 10% 60%, rgba(10, 58, 92, 0.5), transparent);
        }}
        
        .hero-grid {{
            position: absolute;
            inset: 0;
            background-image: 
                linear-gradient(rgba(4, 35, 59, 0.03) 1px, transparent 1px),
                linear-gradient(90deg, rgba(4, 35, 59, 0.03) 1px, transparent 1px);
            background-size: 60px 60px;
            mask-image: radial-gradient(ellipse at center, black 30%, transparent 70%);
        }}
        
        [data-theme="dark"] .hero-grid {{
            background-image: 
                linear-gradient(rgba(255, 255, 255, 0.03) 1px, transparent 1px),
                linear-gradient(90deg, rgba(255, 255, 255, 0.03) 1px, transparent 1px);
        }}
        
        .hero-content {{
            position: relative;
            z-index: 1;
            max-width: 800px;
            margin: 0 auto;
        }}
        
        .hero h1 {{
            font-size: clamp(2.5rem, 6vw, 3.5rem);
            font-weight: 300;
            line-height: 1.2;
            margin-bottom: 24px;
            letter-spacing: -0.01em;
        }}
        
        .highlight {{
            color: var(--accent-golden);
            font-weight: 700;
        }}
        
        .hero-description {{
            font-size: 1.25rem;
            color: var(--text-secondary);
            max-width: 640px;
            margin: 0 auto 40px;
            line-height: 1.7;
        }}
        
        .hero-meta {{
            display: flex;
            justify-content: center;
            gap: 40px;
            flex-wrap: wrap;
            font-size: 0.9rem;
            color: var(--text-tertiary);
        }}
        
        .hero-meta span {{
            display: flex;
            align-items: center;
            gap: 8px;
        }}
        
        .hero-meta svg {{
            width: 18px;
            height: 18px;
            color: var(--accent-golden);
        }}
        
        /* ===================================
           Performance Cards - like stat cards
           =================================== */
        
        .perf-cards {{
            display: flex;
            flex-wrap: wrap;
            gap: 24px;
            justify-content: center;
            max-width: var(--container-max);
            margin: -40px auto 48px;
            padding: 0 24px;
            position: relative;
            z-index: 10;
        }}
        
        .perf-card {{
            flex: 1;
            min-width: 200px;
            max-width: 280px;
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: var(--border-radius-lg);
            padding: 32px;
            text-align: center;
            box-shadow: var(--card-shadow);
            transition: all var(--transition-normal);
        }}
        
        .perf-card:hover {{
            transform: translateY(-8px);
            box-shadow: var(--card-shadow-hover);
            border-color: var(--accent-golden);
        }}
        
        .perf-card .metric {{
            font-size: 2rem;
            font-weight: 700;
            color: var(--accent-golden);
            line-height: 1;
            margin-bottom: 4px;
            font-family: 'JetBrains Mono', monospace;
        }}
        
        .perf-card .label {{
            font-size: 1rem;
            color: var(--text-primary);
            font-weight: 500;
            margin-top: 4px;
        }}
        
        .perf-card .sublabel {{
            font-size: 0.9rem;
            color: var(--text-tertiary);
            margin-top: 4px;
        }}
        
        /* Container */
        .container {{
            max-width: var(--container-max);
            margin: 0 auto;
            padding: 0 24px;
        }}
        
        /* ===================================
           Section Styles - matching website
           =================================== */
        
        .section {{
            margin-bottom: 64px;
        }}
        
        .section-header {{
            text-align: center;
            margin-bottom: 48px;
        }}
        
        .section-header h2 {{
            font-size: clamp(2rem, 4vw, 2.5rem);
            font-weight: 300;
            color: var(--text-primary);
            margin-bottom: 8px;
            letter-spacing: -0.01em;
        }}
        
        .section-header h2 .highlight {{
            font-weight: 700;
        }}
        
        .section-badge {{
            display: inline-block;
            padding: 4px 12px;
            background: var(--accent-golden);
            color: var(--primary-navy);
            font-size: 0.75rem;
            font-weight: 700;
            border-radius: 50px;
            text-transform: uppercase;
            letter-spacing: 0.5px;
            margin-top: 8px;
        }}
        
        /* Chart Card */
        .chart-card {{
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: var(--border-radius);
            padding: 24px;
            box-shadow: var(--card-shadow);
        }}
        
        .chart-container {{
            height: 400px;
            width: 100%;
        }}
        
        /* Table */
        .table-wrapper {{
            overflow-x: auto;
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: var(--border-radius);
            box-shadow: var(--card-shadow);
        }}
        
        table {{
            width: 100%;
            border-collapse: collapse;
            font-size: 0.875rem;
        }}
        
        th, td {{
            padding: 16px;
            text-align: left;
            border-bottom: 1px solid var(--border-color);
        }}
        
        th {{
            background: var(--bg-secondary);
            font-weight: 600;
            color: var(--text-primary);
            position: sticky;
            top: 0;
        }}
        
        tr:hover td {{
            background: rgba(233, 160, 51, 0.05);
        }}
        
        tr.rayforce-row {{
            background: rgba(233, 160, 51, 0.08);
        }}
        
        tr.rayforce-row:hover td {{
            background: rgba(233, 160, 51, 0.15);
        }}
        
        .best-value {{
            color: var(--success);
            font-weight: 600;
        }}
        
        .valid {{
            color: var(--success);
        }}
        
        .invalid {{
            color: var(--error);
        }}
        
        .mono {{
            font-family: 'JetBrains Mono', monospace;
        }}
        
        /* ===================================
           Task Tabs - like project cards
           =================================== */
        
        .task-tabs {{
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
            margin-bottom: 24px;
            justify-content: center;
        }}
        
        .task-tab {{
            background: var(--bg-secondary);
            border: 1px solid var(--border-color);
            color: var(--text-secondary);
            padding: 12px 20px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 0.8rem;
            font-weight: 600;
            transition: all var(--transition-normal);
            font-family: 'JetBrains Mono', monospace;
        }}
        
        .task-tab:hover {{
            border-color: var(--accent-golden);
            color: var(--accent-golden);
            transform: translateY(-2px);
        }}
        
        .task-tab.active {{
            background: var(--accent-golden);
            border-color: var(--accent-golden);
            color: var(--primary-navy);
        }}
        
        .task-panel {{
            display: none;
        }}
        
        .task-panel.active {{
            display: block;
        }}
        
        /* ===================================
           Footer - matching website
           =================================== */
        
        .footer {{
            padding: 80px 0 40px;
            background: var(--primary-navy);
            color: rgba(255, 255, 255, 0.8);
            margin-top: 64px;
        }}
        
        .footer-content {{
            max-width: var(--container-max);
            margin: 0 auto;
            padding: 0 24px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 24px;
        }}
        
        .footer-brand {{
            display: flex;
            align-items: center;
            gap: 12px;
        }}
        
        .footer-logo {{
            height: 36px;
            width: auto;
        }}
        
        .footer-links {{
            display: flex;
            gap: 32px;
        }}
        
        .footer-links a {{
            color: rgba(255, 255, 255, 0.6);
            font-weight: 500;
            transition: color var(--transition-fast);
        }}
        
        .footer-links a:hover {{
            color: var(--accent-golden);
        }}
        
        .footer-copyright {{
            width: 100%;
            text-align: center;
            padding-top: 32px;
            margin-top: 24px;
            border-top: 1px solid rgba(255, 255, 255, 0.1);
            color: rgba(255, 255, 255, 0.5);
            font-size: 0.9rem;
        }}
        
        .footer-copyright a {{
            color: var(--accent-golden);
            transition: opacity var(--transition-fast);
        }}
        
        .footer-copyright a:hover {{
            opacity: 0.8;
        }}
        
        /* Animations */
        @keyframes fadeInUp {{
            from {{
                opacity: 0;
                transform: translateY(20px);
            }}
            to {{
                opacity: 1;
                transform: translateY(0);
            }}
        }}
        
        .animate-in {{
            animation: fadeInUp 0.5s ease forwards;
        }}
        
        /* Responsive */
        @media (max-width: 768px) {{
            :root {{
                --header-height: 64px;
            }}
            
            .hero {{
                padding-top: calc(var(--header-height) + 40px);
                padding-bottom: 60px;
            }}
            
            .perf-cards {{
                padding: 0 16px;
            }}
            
            .perf-card {{
                min-width: 100%;
            }}
            
            .container {{
                padding: 0 16px;
            }}
            
            .footer-content {{
                flex-direction: column;
                text-align: center;
            }}
            
            .footer-links {{
                flex-wrap: wrap;
                justify-content: center;
            }}
        }}
    </style>
</head>
<body>
    <header class="header">
        <div class="header-container">
            <a href="https://rayforcedb.github.io" class="logo-link" target="_blank">
                <img src="images/logo_dark_full.svg" alt="RayforceDB" class="logo logo-light">
                <img src="images/logo_light_full.svg" alt="RayforceDB" class="logo logo-dark">
            </a>
            <nav class="nav">
                <a href="https://core.rayforcedb.com" class="nav-link" target="_blank">Documentation</a>
                <a href="https://github.com/RayforceDB/rayforce" class="nav-link" target="_blank">GitHub</a>
                <button class="theme-toggle" aria-label="Toggle theme">
                    <svg class="sun-icon" xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                        <circle cx="12" cy="12" r="5"></circle>
                        <line x1="12" y1="1" x2="12" y2="3"></line>
                        <line x1="12" y1="21" x2="12" y2="23"></line>
                        <line x1="4.22" y1="4.22" x2="5.64" y2="5.64"></line>
                        <line x1="18.36" y1="18.36" x2="19.78" y2="19.78"></line>
                        <line x1="1" y1="12" x2="3" y2="12"></line>
                        <line x1="21" y1="12" x2="23" y2="12"></line>
                        <line x1="4.22" y1="19.78" x2="5.64" y2="18.36"></line>
                        <line x1="18.36" y1="5.64" x2="19.78" y2="4.22"></line>
                    </svg>
                    <svg class="moon-icon" xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                        <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"></path>
                    </svg>
                </button>
            </nav>
        </div>
    </header>
    
    <section class="hero">
        <div class="hero-bg">
            <div class="hero-gradient"></div>
            <div class="hero-grid"></div>
        </div>
        <div class="hero-content animate-in">
            <h1>Performance <span class="highlight">Benchmarks</span></h1>
            <p class="hero-description">
                Comparing RayforceDB against industry-leading databases using standardized H2OAI benchmark methodology.
            </p>
            <div class="hero-meta">
                <span>
                    <svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="20" x2="18" y2="10"></line><line x1="12" y1="20" x2="12" y2="4"></line><line x1="6" y1="20" x2="6" y2="14"></line></svg>
                    {results.suite_name}
                </span>
                <span>
                    <svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path></svg>
                    {results.dataset_name}
                </span>
                <span>
                    <svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"></circle><polyline points="12 6 12 12 16 14"></polyline></svg>
                    {timestamp}
                </span>
            </div>
        </div>
    </section>
    
    <div class="perf-cards">
        {perf_cards}
    </div>
    
    <main class="container">
        <section class="section">
            <div class="section-header">
                <h2>Overview Comparison</h2>
                <span class="section-badge">Median Time</span>
            </div>
            <div class="chart-card">
                <div id="main-chart" class="chart-container"></div>
            </div>
        </section>
        
        <section class="section">
            <div class="section-header">
                <h2>Detailed Results</h2>
                <span class="section-badge">All Tasks</span>
            </div>
            {results_table}
        </section>
        
        <section class="section">
            <div class="section-header">
                <h2>Task Breakdown</h2>
                <span class="section-badge">Per-Query</span>
            </div>
            {task_sections}
        </section>
    </main>
    
    <footer class="footer">
        <div class="footer-content">
            <div class="footer-brand">
                <img src="images/logo_light_full.svg" alt="RayforceDB" class="footer-logo">
            </div>
            <div class="footer-links">
                <a href="https://core.rayforcedb.com" target="_blank">Documentation</a>
                <a href="https://github.com/RayforceDB/rayforce" target="_blank">GitHub</a>
                <a href="https://rayforcedb.zulipchat.com/join/l33sichu4vp7nf77hgdul4om/" target="_blank">Community</a>
            </div>
            <div class="footer-copyright">
                Based on <a href="https://h2oai.github.io/db-benchmark/" target="_blank">H2OAI Database Benchmark</a> methodology
            </div>
        </div>
    </footer>
    
    <script>
        // Chart data
        const chartData = {json.dumps(chart_data)};
        
        // Theme management - default to light
        const themeToggle = document.querySelector('.theme-toggle');
        
        function setTheme(isDark) {{
            document.documentElement.setAttribute('data-theme', isDark ? 'dark' : 'light');
            localStorage.setItem('theme', isDark ? 'dark' : 'light');
            updateCharts();
        }}
        
        // Initialize theme (default to light)
        const savedTheme = localStorage.getItem('theme');
        if (savedTheme) {{
            setTheme(savedTheme === 'dark');
        }} else {{
            setTheme(false);
        }}
        
        themeToggle.addEventListener('click', () => {{
            const currentTheme = document.documentElement.getAttribute('data-theme');
            setTheme(currentTheme !== 'dark');
        }});
        
        // Chart colors
        const adapterColors = {{
            'rayforce': '#e9a033',
            'duckdb': '#1565C0',
            'kdb': '#00897B',
            'clickhouse': '#FFCA28',
            'polars': '#E91E63',
        }};
        
        function getAdapterColor(name) {{
            const lower = name.toLowerCase();
            return adapterColors[lower] || '#' + Math.floor(Math.random()*16777215).toString(16);
        }}
        
        // ECharts theme
        function getChartTheme() {{
            const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
            return {{
                textColor: isDark ? '#ffffff' : '#04233b',
                axisLineColor: isDark ? 'rgba(255,255,255,0.2)' : 'rgba(0,0,0,0.1)',
                splitLineColor: isDark ? 'rgba(255,255,255,0.1)' : 'rgba(0,0,0,0.05)',
                backgroundColor: 'transparent',
            }};
        }}
        
        // Main comparison chart
        let mainChart = null;
        
        function initMainChart() {{
            const container = document.getElementById('main-chart');
            if (!container) return;
            
            mainChart = echarts.init(container);
            updateMainChart();
            
            window.addEventListener('resize', () => mainChart.resize());
        }}
        
        function updateMainChart() {{
            if (!mainChart || !chartData.comparison) return;
            
            const theme = getChartTheme();
            const data = chartData.comparison;
            
            const series = data.adapters.map(adapter => ({{
                name: adapter,
                type: 'bar',
                data: data.tasks.map(task => {{
                    const entry = data.values.find(v => v.adapter === adapter && v.task === task);
                    return entry ? entry.median_ms : null;
                }}),
                itemStyle: {{
                    color: getAdapterColor(adapter),
                    borderRadius: [4, 4, 0, 0],
                }},
                emphasis: {{
                    itemStyle: {{
                        shadowBlur: 10,
                        shadowColor: 'rgba(0, 0, 0, 0.3)',
                    }}
                }}
            }}));
            
            const option = {{
                backgroundColor: theme.backgroundColor,
                textStyle: {{ color: theme.textColor }},
                tooltip: {{
                    trigger: 'axis',
                    axisPointer: {{ type: 'shadow' }},
                    backgroundColor: 'rgba(0, 0, 0, 0.8)',
                    borderColor: 'transparent',
                    textStyle: {{ color: '#fff' }},
                    formatter: function(params) {{
                        let html = '<div style="font-weight:600;margin-bottom:8px">' + params[0].axisValue + '</div>';
                        params.sort((a, b) => a.value - b.value);
                        params.forEach(p => {{
                            if (p.value !== null) {{
                                html += '<div style="display:flex;justify-content:space-between;gap:24px;margin:4px 0">';
                                html += '<span style="display:flex;align-items:center;gap:6px">';
                                html += '<span style="width:10px;height:10px;border-radius:50%;background:' + p.color + '"></span>';
                                html += p.seriesName + '</span>';
                                html += '<span style="font-weight:600">' + p.value.toFixed(2) + ' ms</span></div>';
                            }}
                        }});
                        return html;
                    }}
                }},
                legend: {{
                    data: data.adapters,
                    bottom: 0,
                    textStyle: {{ color: theme.textColor }},
                    itemStyle: {{ borderWidth: 0 }}
                }},
                grid: {{
                    left: '3%',
                    right: '4%',
                    bottom: '15%',
                    top: '10%',
                    containLabel: true
                }},
                xAxis: {{
                    type: 'category',
                    data: data.tasks,
                    axisLine: {{ lineStyle: {{ color: theme.axisLineColor }} }},
                    axisLabel: {{ color: theme.textColor, rotate: data.tasks.length > 5 ? 30 : 0 }}
                }},
                yAxis: {{
                    type: 'value',
                    name: 'Time (ms)',
                    nameTextStyle: {{ color: theme.textColor }},
                    axisLine: {{ lineStyle: {{ color: theme.axisLineColor }} }},
                    axisLabel: {{ color: theme.textColor }},
                    splitLine: {{ lineStyle: {{ color: theme.splitLineColor }} }},
                    min: 0
                }},
                series: series
            }};
            
            mainChart.setOption(option);
        }}
        
        // Task detail charts
        const taskCharts = {{}};
        
        function initTaskCharts() {{
            if (!chartData.tasks) return;
            
            Object.keys(chartData.tasks).forEach((taskId, idx) => {{
                const containerId = 'task-chart-' + idx;
                const container = document.getElementById(containerId);
                if (!container) return;
                
                const chart = echarts.init(container);
                taskCharts[taskId] = chart;
                updateTaskChart(taskId, chart);
                
                window.addEventListener('resize', () => chart.resize());
            }});
        }}
        
        function updateTaskChart(taskId, chart) {{
            if (!chart || !chartData.tasks[taskId]) return;
            
            const theme = getChartTheme();
            const data = chartData.tasks[taskId];
            
            const series = data.map(d => ({{
                name: d.adapter,
                type: 'boxplot',
                data: [d.boxplot],
                itemStyle: {{ color: getAdapterColor(d.adapter), borderColor: getAdapterColor(d.adapter) }}
            }}));
            
            const option = {{
                backgroundColor: theme.backgroundColor,
                textStyle: {{ color: theme.textColor }},
                tooltip: {{
                    trigger: 'item',
                    backgroundColor: 'rgba(0, 0, 0, 0.8)',
                    borderColor: 'transparent',
                    textStyle: {{ color: '#fff' }},
                }},
                grid: {{
                    left: '10%',
                    right: '10%',
                    bottom: '15%',
                    top: '10%',
                }},
                xAxis: {{
                    type: 'category',
                    data: data.map(d => d.adapter),
                    axisLine: {{ lineStyle: {{ color: theme.axisLineColor }} }},
                    axisLabel: {{ color: theme.textColor }}
                }},
                yAxis: {{
                    type: 'value',
                    name: 'Time (ms)',
                    nameTextStyle: {{ color: theme.textColor }},
                    axisLine: {{ lineStyle: {{ color: theme.axisLineColor }} }},
                    axisLabel: {{ color: theme.textColor }},
                    splitLine: {{ lineStyle: {{ color: theme.splitLineColor }} }}
                }},
                series: series
            }};
            
            chart.setOption(option);
        }}
        
        function updateCharts() {{
            setTimeout(() => {{
                updateMainChart();
                Object.entries(taskCharts).forEach(([taskId, chart]) => {{
                    updateTaskChart(taskId, chart);
                }});
            }}, 50);
        }}
        
        // Task tabs
        function showTask(idx) {{
            document.querySelectorAll('.task-tab').forEach((tab, i) => {{
                tab.classList.toggle('active', i === idx);
            }});
            document.querySelectorAll('.task-panel').forEach((panel, i) => {{
                panel.classList.toggle('active', i === idx);
                if (i === idx) {{
                    const chart = Object.values(taskCharts)[idx];
                    if (chart) chart.resize();
                }}
            }});
        }}
        
        // Initialize
        document.addEventListener('DOMContentLoaded', function() {{
            initMainChart();
            initTaskCharts();
        }});
    </script>
</body>
</html>
"""
    return html


def _prepare_chart_data(results: BenchmarkResults, stats: BenchmarkStatistics) -> dict:
    """Prepare data for JavaScript charts."""
    chart_data = {
        "comparison": None,
        "tasks": {},
    }
    
    if stats.summary_df is not None and len(stats.summary_df) > 0:
        df = stats.summary_df
        
        # Comparison chart data
        chart_data["comparison"] = {
            "adapters": df["adapter"].unique().tolist(),
            "tasks": df["task"].unique().tolist(),
            "values": [
                {
                    "adapter": row["adapter"],
                    "task": row["task"],
                    "median_ms": row["median_ms"],
                }
                for _, row in df.iterrows()
            ],
        }
    
    # Task detail data
    tasks = {}
    for tr in results.task_results:
        if tr.task_id not in tasks:
            tasks[tr.task_id] = []
        
        if tr.timings_ns:
            times_ms = [t / 1_000_000 for t in tr.timings_ns]
            times_ms_sorted = sorted(times_ms)
            n = len(times_ms_sorted)
            
            q1_idx = n // 4
            q3_idx = (3 * n) // 4
            
            tasks[tr.task_id].append({
                "adapter": tr.adapter_name,
                "values": times_ms,
                "boxplot": [
                    min(times_ms),
                    times_ms_sorted[q1_idx] if n > 0 else 0,
                    times_ms_sorted[n // 2] if n > 0 else 0,
                    times_ms_sorted[q3_idx] if n > 0 else 0,
                    max(times_ms),
                ],
            })
    
    chart_data["tasks"] = tasks
    return chart_data


def _build_performance_cards(stats: BenchmarkStatistics) -> str:
    """Build HTML for performance highlight cards."""
    if stats.summary_df is None or len(stats.summary_df) == 0:
        return ""
    
    df = stats.summary_df
    
    cards = []
    
    for task in df["task"].unique():
        task_df = df[df["task"] == task]
        rayforce_time = task_df[task_df["adapter"].str.lower() == "rayforce"]["median_ms"]
        
        if len(rayforce_time) == 0:
            continue
        
        rayforce_time = rayforce_time.iloc[0]
        other_times = task_df[task_df["adapter"].str.lower() != "rayforce"]["median_ms"]
        
        if len(other_times) == 0:
            continue
        
        slowest = other_times.max()
        if slowest > rayforce_time and rayforce_time > 0:
            speedup = slowest / rayforce_time
            if speedup > 1.5:
                cards.append({
                    "metric": f"{speedup:.1f}x",
                    "label": f"Faster {task}",
                    "sublabel": "vs. competitor",
                })
    
    cards = sorted(cards, key=lambda x: float(x["metric"].rstrip("x")), reverse=True)[:3]
    
    if not cards:
        adapters = df["adapter"].unique()
        tasks = df["task"].unique()
        cards = [
            {"metric": str(len(adapters)), "label": "Databases", "sublabel": "compared"},
            {"metric": str(len(tasks)), "label": "Queries", "sublabel": "benchmarked"},
            {"metric": f"{len(df)}", "label": "Data Points", "sublabel": "collected"},
        ]
    
    html_cards = []
    for card in cards:
        html_cards.append(f"""
            <div class="perf-card">
                <div class="metric">{card['metric']}</div>
                <div class="label">{card['label']}</div>
                <div class="sublabel">{card['sublabel']}</div>
            </div>
        """)
    
    return "\n".join(html_cards)


def _build_results_table(stats: BenchmarkStatistics) -> str:
    """Build HTML results table."""
    if stats.summary_df is None or len(stats.summary_df) == 0:
        return "<p>No results available.</p>"
    
    df = stats.summary_df
    best_by_task = df.groupby("task")["median_ms"].min().to_dict()
    
    rows = []
    for _, row in df.iterrows():
        is_best = abs(row["median_ms"] - best_by_task.get(row["task"], float("inf"))) < 0.01
        is_rayforce = row["adapter"].lower() == "rayforce"
        
        median_class = "best-value" if is_best else ""
        valid_class = "valid" if row["valid"] else "invalid"
        valid_text = "✓" if row["valid"] else "✗"
        row_class = "rayforce-row" if is_rayforce else ""
        
        rows_per_sec = f"{row['rows/s']:,.0f}" if row['rows/s'] else "—"
        
        rows.append(f"""
            <tr class="{row_class}">
                <td><strong>{row['task']}</strong></td>
                <td>{row['adapter']}</td>
                <td class="{median_class} mono">{row['median_ms']:.3f}</td>
                <td class="mono">{row['min_ms']:.3f}</td>
                <td class="mono">{row['p95_ms']:.3f}</td>
                <td class="mono">{rows_per_sec}</td>
                <td class="{valid_class}">{valid_text}</td>
            </tr>
        """)
    
    return f"""
        <div class="table-wrapper">
            <table>
                <thead>
                    <tr>
                        <th>Task</th>
                        <th>Database</th>
                        <th>Median (ms)</th>
                        <th>Min (ms)</th>
                        <th>P95 (ms)</th>
                        <th>Rows/sec</th>
                        <th>Valid</th>
                    </tr>
                </thead>
                <tbody>
                    {''.join(rows)}
                </tbody>
            </table>
        </div>
    """


def _build_task_sections(results: BenchmarkResults, stats: BenchmarkStatistics) -> str:
    """Build HTML for task detail sections with tabs."""
    if not results.task_results:
        return "<p>No task results available.</p>"
    
    tasks = {}
    for tr in results.task_results:
        if tr.task_id not in tasks:
            tasks[tr.task_id] = []
        tasks[tr.task_id].append(tr)
    
    if not tasks:
        return "<p>No tasks to display.</p>"
    
    tabs = []
    panels = []
    
    for idx, (task_id, task_results) in enumerate(tasks.items()):
        active_class = "active" if idx == 0 else ""
        tabs.append(f'<button class="task-tab {active_class}" onclick="showTask({idx})">{task_id}</button>')
        
        panels.append(f"""
            <div class="task-panel {active_class}">
                <div class="chart-card">
                    <div id="task-chart-{idx}" class="chart-container" style="height: 300px;"></div>
                </div>
            </div>
        """)
    
    return f"""
        <div class="task-tabs">
            {''.join(tabs)}
        </div>
        {''.join(panels)}
    """


def _results_to_dict(results: BenchmarkResults, stats: BenchmarkStatistics) -> dict:
    """Convert results and stats to JSON-serializable dictionary."""
    return {
        "suite_name": results.suite_name,
        "dataset_name": results.dataset_name,
        "started_at": results.started_at,
        "finished_at": results.finished_at,
        "task_results": [
            {
                "task_id": tr.task_id,
                "adapter_name": tr.adapter_name,
                "timings_ns": tr.timings_ns,
                "row_counts": tr.row_counts,
                "validation_passed": tr.validation_passed,
                "cache_mode": tr.cache_mode,
            }
            for tr in results.task_results
        ],
        "statistics": [
            {
                "task_id": ts.task_id,
                "adapter_name": ts.adapter_name,
                "n_samples": ts.n_samples,
                "min_ms": ts.min_ms,
                "median_ms": ts.median_ms,
                "p95_ms": ts.p95_ms,
                "max_ms": ts.max_ms,
                "std_ms": ts.std_ms,
                "rows_per_sec": ts.rows_per_sec,
            }
            for ts in stats.task_stats
        ],
    }
