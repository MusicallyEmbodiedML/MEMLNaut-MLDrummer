#!/usr/bin/env python3
"""
Breakbeats Downloader Script
===========================

This script extracts download links from the breakbeats_source.html file,
matches them against the samples listed in samples.txt, and downloads
the .wav files to ./wav and .rx2 files to ./rx2 directories.

It also reports any missing files with debug information.
"""

import os
import re
import sys
import requests
from pathlib import Path
from urllib.parse import unquote
from difflib import SequenceMatcher
import time

# Configuration
HTML_FILE = "breakbeats_source.html"
SAMPLES_FILE = "samples.txt"
WAV_DIR = "wav"
RX2_DIR = "rx2"
LOG_FILE = "download_log.txt"

# Create directories if they don't exist
os.makedirs(WAV_DIR, exist_ok=True)
os.makedirs(RX2_DIR, exist_ok=True)

class BreakbeatsDownloader:
    def __init__(self):
        self.html_content = ""
        self.wanted_samples = []
        self.available_links = {}
        self.download_log = []
        self.missing_samples = []

    def load_html(self):
        """Load the HTML content from the file"""
        try:
            with open(HTML_FILE, 'r', encoding='utf-8') as f:
                self.html_content = f.read()
            print(f"✓ Loaded HTML file: {HTML_FILE}")
        except FileNotFoundError:
            print(f"✗ Error: HTML file '{HTML_FILE}' not found!")
            sys.exit(1)
        except Exception as e:
            print(f"✗ Error loading HTML file: {e}")
            sys.exit(1)

    def load_samples_list(self):
        """Load the list of wanted samples from samples.txt"""
        try:
            with open(SAMPLES_FILE, 'r', encoding='utf-8') as f:
                self.wanted_samples = [line.strip() for line in f if line.strip()]
            print(f"✓ Loaded {len(self.wanted_samples)} samples from {SAMPLES_FILE}")
            for sample in self.wanted_samples:
                print(f"  - {sample}")
        except FileNotFoundError:
            print(f"✗ Error: Samples file '{SAMPLES_FILE}' not found!")
            sys.exit(1)
        except Exception as e:
            print(f"✗ Error loading samples file: {e}")
            sys.exit(1)

    def extract_download_links(self):
        """Extract all download links from the HTML"""
        pattern = r'<a href="(https://rhythm-lab\.com/sstorage/[^"]+\.(wav|rx2|WAV|RX2))"[^>]*>([^<]+)</a>'
        matches = re.findall(pattern, self.html_content, re.IGNORECASE)

        for url, ext, filename in matches:
            # Clean up the filename
            clean_filename = filename.strip()
            if clean_filename not in self.available_links:
                self.available_links[clean_filename] = {}

            # Store both URL and extension
            self.available_links[clean_filename][ext.lower()] = url

        print(f"✓ Extracted {len(self.available_links)} unique samples from HTML")

    def similarity(self, a, b):
        """Calculate similarity between two strings"""
        return SequenceMatcher(None, a.lower(), b.lower()).ratio()

    def find_best_match(self, wanted_sample):
        """Find the best matching sample in available links"""
        best_match = None
        best_score = 0

        for available_sample in self.available_links.keys():
            score = self.similarity(wanted_sample, available_sample)
            if score > best_score:
                best_score = score
                best_match = available_sample

        return best_match, best_score

    def normalize_sample_name(self, sample_name):
        """Normalize sample name for better matching"""
        # Remove common variations and normalize
        normalized = sample_name.lower()
        normalized = re.sub(r'\s+', ' ', normalized)  # Multiple spaces to single
        normalized = re.sub(r'[^\w\s-]', '', normalized)  # Remove special chars except dash
        normalized = normalized.strip()
        return normalized

    def match_samples(self):
        """Match wanted samples with available links"""
        matched_samples = {}

        for wanted in self.wanted_samples:
            print(f"\n🔍 Searching for: {wanted}")

            # Try exact match first
            exact_match = None
            for available in self.available_links.keys():
                if self.similarity(wanted, available) > 0.9:  # Very high similarity
                    exact_match = available
                    break

            if exact_match:
                matched_samples[wanted] = exact_match
                print(f"  ✓ Found exact match: {exact_match}")
            else:
                # Try fuzzy matching
                best_match, score = self.find_best_match(wanted)
                if score > 0.6:  # Reasonable similarity threshold
                    matched_samples[wanted] = best_match
                    print(f"  ∼ Found fuzzy match ({score:.2f}): {best_match}")
                else:
                    self.missing_samples.append({
                        'wanted': wanted,
                        'best_match': best_match,
                        'score': score
                    })
                    print(f"  ✗ No good match found (best: {best_match}, score: {score:.2f})")

        return matched_samples

    def download_file(self, url, filepath):
        """Download a file from URL to filepath"""
        try:
            print(f"    Downloading: {os.path.basename(filepath)}")

            # Check if file already exists
            if os.path.exists(filepath):
                print(f"    ⚠ File already exists, skipping: {os.path.basename(filepath)}")
                return True

            response = requests.get(url, stream=True, timeout=30)
            response.raise_for_status()

            with open(filepath, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    f.write(chunk)

            file_size = os.path.getsize(filepath)
            print(f"    ✓ Downloaded: {os.path.basename(filepath)} ({file_size:,} bytes)")
            return True

        except requests.exceptions.RequestException as e:
            print(f"    ✗ Download failed: {e}")
            return False
        except Exception as e:
            print(f"    ✗ Error: {e}")
            return False

    def download_matched_samples(self, matched_samples):
        """Download all matched samples"""
        print(f"\n📥 Starting downloads for {len(matched_samples)} matched samples...")

        success_count = 0
        total_files = 0

        for wanted, matched in matched_samples.items():
            print(f"\n🎵 Processing: {wanted}")
            print(f"  → Matched with: {matched}")

            if matched in self.available_links:
                links = self.available_links[matched]

                # Download WAV file
                if 'wav' in links:
                    total_files += 1
                    safe_filename = re.sub(r'[<>:"/\\|?*]', '_', matched)
                    if not safe_filename.endswith('.wav'):
                        safe_filename += '.wav'
                    filepath = os.path.join(WAV_DIR, safe_filename)

                    if self.download_file(links['wav'], filepath):
                        success_count += 1
                        self.download_log.append(f"SUCCESS: {wanted} -> {safe_filename} (WAV)")
                    else:
                        self.download_log.append(f"FAILED: {wanted} -> {safe_filename} (WAV)")

                # Download RX2 file
                if 'rx2' in links:
                    total_files += 1
                    safe_filename = re.sub(r'[<>:"/\\|?*]', '_', matched)
                    if not safe_filename.endswith('.rx2'):
                        safe_filename += '.rx2'
                    filepath = os.path.join(RX2_DIR, safe_filename)

                    if self.download_file(links['rx2'], filepath):
                        success_count += 1
                        self.download_log.append(f"SUCCESS: {wanted} -> {safe_filename} (RX2)")
                    else:
                        self.download_log.append(f"FAILED: {wanted} -> {safe_filename} (RX2)")

                # Add small delay between downloads to be respectful
                time.sleep(0.5)

        print(f"\n📊 Download Summary:")
        print(f"  Successfully downloaded: {success_count}/{total_files} files")
        print(f"  Success rate: {success_count/total_files*100:.1f}%" if total_files > 0 else "  No files to download")

    def generate_report(self):
        """Generate a detailed report of the download process"""
        print(f"\n📋 Generating report...")

        report = []
        report.append("BREAKBEATS DOWNLOAD REPORT")
        report.append("=" * 50)
        report.append(f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        report.append("")

        # Download log
        report.append("DOWNLOAD LOG:")
        report.append("-" * 20)
        for log_entry in self.download_log:
            report.append(log_entry)
        report.append("")

        # Missing samples
        if self.missing_samples:
            report.append("MISSING SAMPLES:")
            report.append("-" * 20)
            for missing in self.missing_samples:
                report.append(f"WANTED: {missing['wanted']}")
                report.append(f"  Best match: {missing['best_match']}")
                report.append(f"  Similarity: {missing['score']:.2f}")
                report.append(f"  Reason: Similarity too low (< 0.6)")
                report.append("")

        # Available samples (for debugging)
        report.append("ALL AVAILABLE SAMPLES:")
        report.append("-" * 25)
        for sample in sorted(self.available_links.keys()):
            formats = list(self.available_links[sample].keys())
            report.append(f"{sample} ({', '.join(formats)})")

        # Write report to file
        with open(LOG_FILE, 'w', encoding='utf-8') as f:
            f.write('\n'.join(report))

        print(f"✓ Report saved to: {LOG_FILE}")

        # Print summary to console
        print(f"\n🎯 FINAL SUMMARY:")
        print(f"  Total wanted samples: {len(self.wanted_samples)}")
        print(f"  Successfully matched: {len(self.wanted_samples) - len(self.missing_samples)}")
        print(f"  Missing samples: {len(self.missing_samples)}")

        if self.missing_samples:
            print(f"\n❌ Missing samples:")
            for missing in self.missing_samples:
                print(f"  - {missing['wanted']}")
                print(f"    Best match: {missing['best_match']} (similarity: {missing['score']:.2f})")

    def run(self):
        """Run the complete download process"""
        print("🎼 BREAKBEATS DOWNLOADER")
        print("=" * 30)

        # Load files
        self.load_html()
        self.load_samples_list()

        # Extract and match
        self.extract_download_links()
        matched_samples = self.match_samples()

        # Download
        if matched_samples:
            self.download_matched_samples(matched_samples)
        else:
            print("\n❌ No samples matched! Check the samples.txt file and HTML content.")

        # Generate report
        self.generate_report()

        print(f"\n✅ Process complete! Check the '{WAV_DIR}' and '{RX2_DIR}' directories for downloaded files.")

if __name__ == "__main__":
    downloader = BreakbeatsDownloader()
    downloader.run()
