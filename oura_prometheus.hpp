#ifndef OURA_PROMETHEUS_LIB_H
#define OURA_PROMETHEUS_LIB_H

#include <string>
#include <atomic>
#include <set>
#include <map>
#include <memory>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <regex>
#include <mutex>

namespace oura_prometheus
{
    //////////////////////////////////////////////////////
    //// API GENERICS
    //////////////////////////////////////////////////////

    // std::atomic<double> wrapper class that support += and -= operators
    class atomic_double : public std::atomic<double> 
    {
    public:
        using std::atomic<double>::atomic;
        using std::atomic<double>::operator=;
        
        atomic_double(const atomic_double&d)
        {
            change(d.load());
        }

        atomic_double& operator+=(const atomic_double& value)
        {
            return change(this->load() + value);
        }

        atomic_double& operator-=(const atomic_double& value)
        {
            return change(this->load() - value);
        }

        atomic_double& change(double target_value)
        {
            for (double current = *this; !this->compare_exchange_weak(current, target_value););
            return *this;
        }
    };

    // Enumeration that list all prometheus available metric types
    // (https://prometheus.io/docs/concepts/metric_types/)
    enum class MetricType
    {
        Counter,
        Gauge,
        Summary,
        Histogram
    };

    // Conversion function of a metric type value to it's string representation
    std::string metric_type_to_string(MetricType type)
    {
        std::string value = "";
        switch (type)
        {
        case MetricType::Counter:
            value = "counter";
            break;
        case MetricType::Gauge:
            value = "gauge";
            break;
        case MetricType::Summary:
            value = "summary";
            break;
        case MetricType::Histogram:
            value = "histogram";
            break;
        }
        return value;
    }

    const std::regex name_format_regex("[a-zA-Z_:][a-zA-Z0-9_:]*");
    void check_name_format(const std::string& name)
    {
        if(!std::regex_match(name, name_format_regex))
            throw std::invalid_argument("Metric name does not follow format");
    }

    struct Label
    {
        const std::string name;
        const std::string value;

        bool operator<(const Label &label) const
        {
            return std::tie(name, value) < std::tie(label.name, label.value);
        }

        bool operator==(const Label &label) const
        {
            return std::tie(name, value) == std::tie(label.name, label.value);
        }
    };

    using MetricSerializer = std::function<void(std::ostream&, const std::string&, double, const std::set<Label>&, Label)>;

    class Metric
    {
    public:
        Metric(const std::string &name, const std::string &description, MetricType type)
            : _name(name), _description(description), _type(type)
        {
            check_name_format(name);
        }

        const std::string& get_name() const
        {
            return _name;
        }

        const std::string& get_description() const
        {
            return _description;
        }

        const MetricType get_type() const
        {
            return _type;
        }

        virtual void serialize(std::ostream& stream, const MetricSerializer& serializer) const = 0;

    protected:
        const std::string _name;
        const std::string _description;
        const MetricType _type;
    };

    class Serializer
    {
    public:
        virtual void serialize(std::ostream& stream, std::map<std::string, std::weak_ptr<Metric>>& metrics) = 0;
    };

    class Collectable
    {
    public:
        virtual void collect(std::map<std::string, std::weak_ptr<Metric>>& metrics) = 0;
    };

    class Registry : public Collectable
    {
    public:
        bool register_metric(std::shared_ptr<Metric> metric)
        {
            std::lock_guard<std::mutex> lock(_access_mtx);
            bool was_metric_registered = false;
            const std::string& metric_name = metric->get_name();
            if(_register_metrics.find(metric_name) == _register_metrics.end())
            {
                was_metric_registered = true;
                _register_metrics.insert({metric_name, metric});
            }
            return was_metric_registered;
        }

        bool unregister_metric(const std::string& metric_name)
        {
            std::lock_guard<std::mutex> lock(_access_mtx);
            return _register_metrics.erase(metric_name) != 0;
        }

        std::shared_ptr<Metric> get_metric(const std::string& metric_name)
        {
            std::lock_guard<std::mutex> lock(_access_mtx);
            std::shared_ptr<Metric> metric = nullptr;
            if(_register_metrics.find(metric_name) != _register_metrics.end())
            {
                metric = _register_metrics[metric_name];
            }
            return metric;
        }

        std::size_t size()
        {
            std::lock_guard<std::mutex> lock(_access_mtx);
            return _register_metrics.size();
        }

        virtual void collect(std::map<std::string, std::weak_ptr<Metric>>& metrics) override
        {
            for(const auto& p : _register_metrics)
            {
                metrics.insert({p.first, p.second});
            }
        }

    protected:
        std::mutex _access_mtx;
        std::map<std::string, std::shared_ptr<Metric>> _register_metrics = {};
    };

    const std::regex label_name_format_regex("[a-zA-Z_][a-zA-Z0-9_]*");
    void check_label_name_format(const std::string& label_name)
    {
        if(!std::regex_match(label_name, label_name_format_regex))
            throw std::invalid_argument("Label name does not follow format");
    }

    template <typename T>
    class MetricFamily : public Metric
    {
    public:
        MetricFamily(const std::string &name, const std::string &description, MetricType type, const std::set<std::string> &labels_names)
            : Metric(name, description, type), _labels_names(labels_names)
        {
            std::for_each(labels_names.begin(), labels_names.end(), check_label_name_format);
        }

    protected:
        template <typename... Args>
        std::shared_ptr<T> labels(const std::set<Label> &labels, Args &&... args)
        {
            // Verify label set size
            if (labels.size() != _labels_names.size())
            {
                throw std::invalid_argument("Incorrect number of label given");
            }

            // Verify label set names
            auto pred = [&](const Label& l){return _labels_names.find(l.name) == _labels_names.end();};
            if(std::any_of(labels.begin(), labels.end(), pred))
            {
                throw std::invalid_argument("Incorrect label combinaison given");
            }

            std::shared_ptr<T> res;
            // Return metric if label combinaison already exists
            if(_metrics.find(labels) != _metrics.end())
            {
                res = _metrics[labels];
            }
            // Or create and return a new metric
            else
            {
                std::shared_ptr<T> new_metric = std::make_shared<T>(args...);
                _metrics.insert({labels, new_metric});
                res = new_metric;
            }
            return res;
        }

        virtual void serialize(std::ostream& stream, const MetricSerializer& serializer) const override
        {
            for(const auto& p : _metrics)
            {
                p.second->serialize(stream, serializer, _name, p.first);
            }
        }

    protected:
        const std::set<std::string> _labels_names;
        std::map<std::set<Label>, std::shared_ptr<T>> _metrics = {};
    };

    //////////////////////////////////////////////////////
    //// COUNTER METRIC
    //////////////////////////////////////////////////////
    class Counter
    {
    public:
        explicit Counter() : _value(0) {}
        double get() {return _value;}
        void inc() { _value += 1; }
        void add(const double value)
        {
            if (value > 0.0)
            {
                _value += value;
            }
        }

        void serialize(std::ostream& stream, const MetricSerializer& serializer, const std::string& name, const std::set<Label>& labels = {}) const
        {
            serializer(stream, name, _value, labels, {"",""});
        }

    protected:
        atomic_double _value;
    };

    class CounterMetric : public Metric, public Counter
    {
    public:
        CounterMetric(const std::string &name, const std::string &description)
            : Metric(name, description, MetricType::Counter), Counter()
        {
        }

        virtual void serialize(std::ostream& stream, const MetricSerializer& serializer) const override
        {
            Counter::serialize(stream, serializer, _name);
        }
    };

    class CounterFamily : public MetricFamily<Counter>
    {
    public:
        CounterFamily(const std::string& name, const std::string& description, const std::set<std::string>& labels_names)
            : MetricFamily(name, description, MetricType::Counter, labels_names)
        {
        }

        std::shared_ptr<Counter> labels(const std::set<Label> &labels)
        {
            return MetricFamily::labels(labels);
        }
    };

    //////////////////////////////////////////////////////
    //// GAUGE METRIC
    //////////////////////////////////////////////////////
    class Gauge
    {
    public:
        explicit Gauge(const double initial_value = 0) : _value(initial_value) {}
        double get() {return _value;}
        void set(const double value) { _value = value; }
        void inc() { _value += 1; }
        void dec() { _value -= 1; }
        void add(const double value)
        {
            if (value > 0.0)
            {
                _value += value;
            }
        }
        void sub(const double &value)
        {
            if (value > 0.0)
            {
                _value -= value;
            }
        }

        void serialize(std::ostream& stream, const MetricSerializer& serializer, const std::string& name, const std::set<Label>& labels = {}) const
        {
            serializer(stream, name, _value, labels, {"",""});
        }

    protected:
        atomic_double _value;
    };

    class GaugeMetric : public Metric, public Gauge
    {
    public:
        GaugeMetric(const std::string &name, const std::string &description, const double initial_value = 0)
            : Metric(name, description, MetricType::Gauge), Gauge(initial_value)
        {
        }

        virtual void serialize(std::ostream& stream, const MetricSerializer& serializer) const override
        {
            Gauge::serialize(stream, serializer, _name);
        }
    };

    class GaugeFamily : public MetricFamily<Gauge>
    {
    public:
        GaugeFamily(const std::string &name, const std::string &description, const std::set<std::string> &labels_names)
            : MetricFamily(name, description, MetricType::Gauge, labels_names)
        {
        }

        std::shared_ptr<Gauge> labels(const std::set<Label> &labels, const double default_value = 0)
        {
            return MetricFamily::labels(labels, default_value);
        }
    };

    //////////////////////////////////////////////////////
    //// HISTOGRAM METRIC
    //////////////////////////////////////////////////////
    const std::set<double> default_buckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};

    class Histogram
    {
    public:
        explicit Histogram(const std::set<double>& buckets = default_buckets)
            : _sum(0)
        {
            // Creating buckets
            _buckets.insert({std::numeric_limits<double>::infinity(), 0});
            for(const auto& bucket : buckets)
            {
                _buckets.insert({bucket, 0});
            }
        }

        void observe(const double value)
        {
            _sum += value;
            for(auto& bucket : _buckets)
            {
                if(value <= bucket.first)
                {
                    bucket.second += 1;
                }
            }
        }

        const std::map<double, atomic_double>& buckets() const
        {
            return _buckets;
        }

        void serialize(std::ostream& stream, const MetricSerializer& serializer, const std::string& name, const std::set<Label>& labels = {}) const
        {
            for(const auto& p : _buckets)
            {
                serializer(stream, name, p.second, labels, {"le", std::to_string(p.first)});
            }
        }

    protected:
        std::map<double, atomic_double> _buckets;
        atomic_double _sum;
    };

    class HistogramMetric : public Metric, public Histogram
    {
    public:
        HistogramMetric(const std::string &name, const std::string &description, const std::set<double>& buckets = default_buckets)
            : Metric(name, description, MetricType::Histogram), Histogram(buckets)
        {
        }

        virtual void serialize(std::ostream& stream, const MetricSerializer& serializer) const override
        {
            Histogram::serialize(stream, serializer, _name);
        }
    };

    class HistogramFamily : public MetricFamily<Histogram>
    {
    public:
        HistogramFamily(const std::string &name, const std::string &description, const std::set<std::string> &labels_names, const std::set<double>& buckets = default_buckets)
            : MetricFamily(name, description, MetricType::Gauge, labels_names), _buckets(buckets)
        {
        }

        std::shared_ptr<Histogram> labels(const std::set<Label> &labels)
        {
            return MetricFamily::labels(labels, _buckets);
        }

    protected:
        const std::set<double> _buckets;
    };
    
    inline std::string escape_double_quotes(const std::string& text)
    {
        return std::regex_replace(text, std::regex("\""), "\\\"");
    }

    class TextSerializer : public Serializer
    {
    public:
        virtual void serialize(std::ostream& stream, std::map<std::string, std::weak_ptr<Metric>>& metrics) override
        {
            for(const auto& p : metrics)
            {
                if(auto metric = p.second.lock())
                {
                    stream << "# HELP " << metric->get_name() << " " << metric->get_description() << std::endl;
                    stream << "# TYPE " << metric->get_name() << " " << metric_type_to_string(metric->get_type()) << std::endl;
                    metric->serialize(stream, TextSerializer::metric_serializer);
                }
            }
        }

       static void metric_serializer(std::ostream& stream,
                                     const std::string& name, 
                                     double value, 
                                     const std::set<Label>& labels, 
                                     Label additional_label)
        {
            stream << name;
            if(!labels.empty() || !additional_label.name.empty())
            {
                stream << "{";
                std::string comma = "";
                for(const auto& label : labels)
                {
                    stream << comma << label.name << "=\"" << escape_double_quotes(label.value) << "\"";
                    comma = ",";
                }
                if(!additional_label.name.empty())
                {
                    stream << comma << additional_label.name << "=\"" << escape_double_quotes(additional_label.value) << "\"";
                }
               stream << "}";
            }
            stream << " " << value << std::endl;
        }

    };

} // namespace oura_prometheus

#endif