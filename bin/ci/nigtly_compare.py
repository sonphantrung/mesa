#!/usr/bin/env python3
# Copyright © 2020 - 2024 Collabora Ltd.
# Authors:
#   David Heidelberg <david.heidelberg@collabora.com>
#   Sergi Blanch Torne <sergi.blanch.torne@collabora.com>
# SPDX-License-Identifier: MIT

"""
Compare the two latest scheduled pipelines and provide information
about the jobs you're interested in.
"""

import argparse
import re

import gitlab
from colorama import Fore, Style
from gitlab_common import read_token


MARGE_BOT_USER_ID = 9716


def parse_args() -> None:
    """Parse args"""
    parser = argparse.ArgumentParser(
        description="Tool to show merge requests assigned to the marge-bot",
    )
    parser.add_argument(
        "--target",
        metavar="target-job",
        help="Target job regex. For multiple targets, pass multiple values, "
        "eg. `--target foo bar`.",
        required=False,
        nargs=argparse.ONE_OR_MORE,
    )
    parser.add_argument(
        "--token",
        metavar="token",
        help="force GitLab token, otherwise it's read from ~/.config/gitlab-token",
    )
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    token = read_token(args.token)
    gl = gitlab.Gitlab(url="https://gitlab.freedesktop.org", private_token=token)

    project = gl.projects.get("mesa/mesa")

    print(
        "\u001b]8;;https://gitlab.freedesktop.org/mesa/mesa/-/pipelines?page=1&scope=all&source=schedule\u001b\\Scheduled pipelines overview\u001b]8;;\u001b\\"
    )
    pipelines = project.pipelines.list(
        source="schedule", ordered_by="created_at", sort="desc", page=1, per_page=2
    )
    print(
        f"Old pipeline: {pipelines[1].created_at}\t\u001b]8;;{pipelines[1].web_url}\u001b\\{pipelines[1].status}\u001b]8;;\u001b\\\t{pipelines[1].sha}"
    )
    print(
        f"New pipeline: {pipelines[0].created_at}\t\u001b]8;;{pipelines[0].web_url}\u001b\\{pipelines[0].status}\u001b]8;;\u001b\\\t{pipelines[0].sha}"
    )
    print(
        f"\nWebUI visual compare: https://gitlab.freedesktop.org/mesa/mesa/-/compare/{pipelines[1].sha}...{pipelines[0].sha}\n"
    )

    # regex part
    if args.target:
        target = "|".join(args.target)
        target = target.strip()
        print("🞋 jobs: " + Fore.BLUE + target + Style.RESET_ALL)

        target = f"({target})" + r"( \d+/\d+)?"
    else:
        target = ".*"

    target_jobs_regex: re.Pattern = re.compile(target)

    old_failed_jobs = []
    for job in pipelines[1].jobs.list(all=True):
        if (
            job.status != "failed"
            or target_jobs_regex
            and not target_jobs_regex.fullmatch(job.name)
        ):
            continue
        old_failed_jobs.append(job)

    job_failed = False
    for job in pipelines[0].jobs.list(all=True):
        if (
            job.status != "failed"
            or target_jobs_regex
            and not target_jobs_regex.fullmatch(job.name)
        ):
            continue

        previously_failed = False

        job_failed = True

        for old_job in old_failed_jobs:
            if job.name == old_job.name:
                previously_failed = True
                print(
                    Fore.YELLOW
                    + f"\t\u001b]8;;{job.web_url}\u001b\\{job.name}\u001b]8;;\u001b\\"
                    + Fore.MAGENTA
                    + f" \u001b]8;;{old_job.web_url}\u001b\\(previous run)\u001b]8;;\u001b\\"
                    + Style.RESET_ALL
                )
                break
        if not previously_failed:
            print(
                Fore.RED
                + f"\t\u001b]8;;{job.web_url}\u001b\\{job.name}\u001b]8;;\u001b\\"
                + Style.RESET_ALL
            )

    if not job_failed:
        exit(0)

    commit = project.commits.get(pipelines[0].sha)
    while True:
        print(
            f"{commit.id}  \u001b]8;;{commit.web_url}\u001b\\{commit.title}\u001b]8;;\u001b\\"
        )
        if commit.id == pipelines[1].sha:
            break
        commit = project.commits.get(commit.parent_ids[0])
