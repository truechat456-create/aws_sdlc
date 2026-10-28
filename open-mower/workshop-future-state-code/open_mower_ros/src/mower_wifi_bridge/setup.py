## catkin python package setup for mower_wifi_bridge (FR-9).
## Do not invoke directly; this is consumed by catkin via catkin_python_setup().
from setuptools import setup
from catkin_pkg.python_setup import generate_distutils_setup

setup_args = generate_distutils_setup(
    packages=["mower_wifi_bridge"],
    package_dir={"": "src"},
)

setup(**setup_args)
