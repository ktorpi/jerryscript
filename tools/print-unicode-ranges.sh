#!/bin/bash

# Copyright 2015 Samsung Electronics Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# http://www.unicode.org/Public/3.0-Update/UnicodeData-3.0.0.txt
#

# unicode categories:      Lu Ll Lt Mn Mc Me Nd Nl No Zs Zl Zp Cc Cf Cs Co Lm Lo Pc Pd Ps Pe Pi Pf Po Sm Sc Sk So
# letter:                  Lu Ll Lt Lm Lo Nl
# non-leter-indent-part:
#   digit:                 Nd
#   punctuation mark:      Mn Mc
#   connector punctuation: Pc
# separators:              Zs

if [ $# -ne 5 ]; then
  echo "useage: print-unicode-ranges.sh <unicode-data-path> <-i y|n> <-cat letters|non-let-indent-parts|separators>"
  echo "  -i:   y|n whether print intervals or individual chars"
  echo "  -cat: whether print letters|non-let-indent-parts|separators category"
  exit 1
fi

UNICODE_DATA_PATH="$1"
shift

while [ $# -gt 0 ]; do
  if [ $1 == "-i" ]; then
    shift
    PRINT_INTERVALS="$1"
  elif [ $1 == "-cat" ]; then
   shift
   CATEGORY="$1"
   echo $CATEGORY
  fi
  shift
done

awk -v desired_category="$CATEGORY" \
'BEGIN { FS=";"; OFS=";" } { \
 cat=$3; \
 if (desired_category=="letters" && (cat=="Lu"||cat=="Ll"||cat=="Lt"||cat=="Lm"||cat=="Lo"||cat=="Nl")) \
 { \
    print "0x"$1, $2, $3; \
 } \
 else if (desired_category=="non-let-indent-parts" && (cat=="Nd"||cat=="Mn"||cat=="Mc"||cat=="Pc")) \
 { \
    print "0x"$1, $2, $3; \
 } \
 else if (desired_category=="separators" && cat=="Zs") \
 { \
    print "0x"$1, $2, $3; \
 } \
}' $UNICODE_DATA_PATH \
| awk --non-decimal-data -v print_intervals="$PRINT_INTERVALS" \
'BEGIN \
 { \
   FS=";"; \
   OFS=";"; \
   is_in_range=0; \
 } \
 \
 function output_next_range () \
 { \
   if (range_begin != range_prev && print_intervals=="y") \
   { \
     printf "{ %s, %s }, \\\n", range_begin, range_prev; \
   } \
   else if (range_begin == range_prev && print_intervals!="y")\
   { \
     printf "%s, \\\n", range_begin; \
   } \
 } \
 \
 { \
   if (is_in_range == 0) \
   { \
     is_in_range=1; \
     range_begin=$1; \
     range_prev=$1; \
     range_begin_name=$2; \
     range_prev_name=$2; \
   } \
   else \
   { \
     if (range_prev + 1 == $1) \
     { \
       range_prev=$1; \
       range_prev_name=$2
     } \
     else \
     { \
       output_next_range(); \
       range_begin=$1; \
       range_prev=$1; \
       range_begin_name=$2; \
       range_prev_name=$2; \
     } \
   } \
 } \
 \
 END \
 { \
   output_next_range(); \
 }'
