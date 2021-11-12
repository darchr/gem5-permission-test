# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
This runs simple tests to ensure the examples in `configs/example/gem5_library`
still function. They simply check the simulation completed.
"""
from testlib import *
import re
import os

hello_verifier = verifier.MatchRegex(re.compile(r"Hello world!"))

gem5_verify_config(
    name="test-gem5-library-example-arm-hello",
    fixtures=(),
    verifiers=(hello_verifier,),
    config=joinpath(
        config.base_dir,
        "configs",
        "example",
        "gem5_library",
        "arm-hello.py",
    ),
    config_args=[],
    valid_isas=(constants.arm_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.quick_tag,
)


if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-ubuntu-run",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-ubuntu-run.py",
        ),
        config_args=[],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-npb-benchmarks",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-npb-benchmarks.py",
        ),
        config_args=["--benchmark","cg.A.x"],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-parsec-benchmarks",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-parsec-benchmarks.py",
        ),
        config_args=["--benchmark","blackscholes", "--size","simsmall"],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-spec-cpu2006-benchmarks",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-spec-cpu2006-benchmarks.py",
        ),
        config_args=["--benchmark","410.bwaves", "--size","test"],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-spec-cpu2017-benchmarks",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-spec-cpu2017-benchmarks.py",
        ),
        config_args=["--benchmark","503.bwaves_r", "--size","test"],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

if os.access("/dev/kvm", mode=os.R_OK | os.W_OK):
    # The x86-ubuntu-run uses KVM cores, this test will therefore only be run
    # on systems that support KVM.
    gem5_verify_config(
        name="test-gem5-library-example-x86-gapbs-benchmarks",
        fixtures=(),
        verifiers=(),
        config=joinpath(
            config.base_dir,
            "configs",
            "example",
            "gem5_library",
            "x86-gapbs-benchmarks.py",
        ),
        config_args=["--benchmark","bfs",\
                    "--synthetic","1",\
                    "--size","1"],
        valid_isas=(constants.x86_tag,),
        protocol="MESI_Two_Level",
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )


