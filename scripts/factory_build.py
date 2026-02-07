Import("env")
import os

# Change source directory to src_factory for factory environment
project_dir = env.Dir("$PROJECT_DIR").get_abspath()
env.Replace(PROJECT_SRC_DIR=os.path.join(project_dir, "src_factory"))
