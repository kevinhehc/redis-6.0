/* latency.h -- latency monitor API header file
 * See latency.c for more information.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __LATENCY_H
#define __LATENCY_H

#define LATENCY_TS_LEN 160 /* History length for every monitored event. 
                            *
                            * 每个监控事件的历史记录长度。
                            * */

/* Representation of a latency sample: the sampling time and the latency
 * observed in milliseconds. 
 *
 * 延迟样本的表示：采样时间和观察到的延迟（以毫秒为单位）。
 * */
struct latencySample {
    int32_t time; /* We don't use time_t to force 4 bytes usage everywhere. 
                   *
                   * 我们不使用time_t强制在任何地方使用4字节。
                   * */
    uint32_t latency; /* Latency in milliseconds. 
                       *
                       * 延迟（以毫秒为单位）。
                       * */
};

/* The latency time series for a given event. 
 *
 * 给定事件的延迟时间序列。
 * */
struct latencyTimeSeries {
    int idx; /* Index of the next sample to store. 
              *
              * 要存储的下一个样本的索引。
              * */
    uint32_t max; /* Max latency observed for this event. 
                   *
                   * 观察到此事件的最大延迟。
                   * */
    struct latencySample samples[LATENCY_TS_LEN]; /* Latest history. 
                                                   *
                                                   * 最新历史。
                                                   * */
};

/* Latency statistics structure. 
 *
 * 延迟统计结构。
 * */
struct latencyStats {
    uint32_t all_time_high; /* Absolute max observed since latest reset. 
                             *
                             * 自最近一次重置以来观察到的绝对最大值。
                             * */
    uint32_t avg;           /* Average of current samples. 
                             *
                             * 当前样本的平均值。
                             * */
    uint32_t min;           /* Min of current samples. 
                             *
                             * 当前样本的最小值。
                             * */
    uint32_t max;           /* Max of current samples. 
                             *
                             * 当前采样的最大值。
                             * */
    uint32_t mad;           /* Mean absolute deviation. 
                             *
                             * 平均绝对偏差。
                             * */
    uint32_t samples;       /* Number of non-zero samples. 
                             *
                             * 非零样本数。
                             * */
    time_t period;          /* Number of seconds since first event and now. 
                             *
                             * 从第一个事件到现在的秒数。
                             * */
};

void latencyMonitorInit(void);
void latencyAddSample(const char *event, mstime_t latency);
int THPIsEnabled(void);

/* Latency monitoring macros. 
 *
 * 延迟监视宏。
 * */

/* Start monitoring an event. We just set the current time. 
 *
 * 开始监视事件。我们只是设置了当前时间。
 * */
#define latencyStartMonitor(var) if (server.latency_monitor_threshold) { \
    var = mstime(); \
} else { \
    var = 0; \
}

/* End monitoring an event, compute the difference with the current time
 * to check the amount of time elapsed. 
 *
 * 结束对事件的监视，计算与当前时间的差值，以检查经过的时间量。
 * */
#define latencyEndMonitor(var) if (server.latency_monitor_threshold) { \
    var = mstime() - var; \
}

/* Add the sample only if the elapsed time is >= to the configured threshold. 
 *
 * 仅当运行时间>=配置的阈值时，才添加样本。
 * */
#define latencyAddSampleIfNeeded(event,var) \
    if (server.latency_monitor_threshold && \
        (var) >= server.latency_monitor_threshold) \
          latencyAddSample((event),(var));

/* Remove time from a nested event. 
 *
 * 从嵌套事件中删除时间。
 * */
#define latencyRemoveNestedEvent(event_var,nested_var) \
    event_var += nested_var;

#endif /* __LATENCY_H */
