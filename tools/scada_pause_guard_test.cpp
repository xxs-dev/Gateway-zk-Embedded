#include "local_display_qt_scada_scene.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QDateTime>
#include <QInputDialog>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace {
const std::string modeTag = "EMS_CORE.local_control_mode";
const std::string appliedTag = "EMS_CORE.local_control_applied";
const std::string powerTag = "EMS_CORE.local_manual_a_kw";
const std::string stopTag = "PCS.stop";
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeRuntime final : public ScadaSceneRuntimeSource {
public:
    std::string node = "edge-1";
    std::unordered_map<std::string, ScadaSceneResolvedTag> tags;
    std::unordered_map<std::string, edge_gateway::StoredPointValue> values;
    std::vector<edge_gateway::PendingWriteCommand> writes;
    FakeRuntime() {
        for (const auto& entry : std::vector<std::pair<std::string, unsigned>>{
                 {modeTag,740100},{appliedTag,740105},{powerTag,740101},{stopTag,1202}}) {
            ScadaSceneResolvedTag resolved;
            resolved.tag.tagId=entry.first; resolved.tag.nodeId=node;
            resolved.tag.displayName=entry.first;
            resolved.mapping.tagId=entry.first; resolved.mapping.nodeId=node;
            resolved.mapping.index=entry.second; resolved.mapping.writable=true;
            resolved.writeMinValue=-41.6; resolved.writeMaxValue=41.6; resolved.writeStep=0.1;
            tags[entry.first]=resolved;
            edge_gateway::StoredPointValue value;
            value.index=entry.second; value.ts=QDateTime::currentMSecsSinceEpoch();
            values[entry.first]=value;
        }
    }
    const std::string& nodeId() const override { return node; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tag) const override {
        const auto found=tags.find(tag);
        if(found==tags.end()) return edge_gateway::NullOpt;
        return found->second;
    }
    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(const std::string& tag, std::int64_t) const override {
        const auto found=values.find(tag);
        if(found==values.end()) return edge_gateway::NullOpt;
        return found->second;
    }
    std::vector<edge_gateway::StoredPointValue> readIndexes(const std::vector<std::uint32_t>& indexes, std::int64_t) const override {
        std::vector<edge_gateway::StoredPointValue> out;
        for(auto index:indexes) for(const auto& value:values) if(value.second.index==index) out.push_back(value.second);
        return out;
    }
    ScadaSceneWriteResult submitWrite(const std::string& tag, edge_gateway::PendingWriteCommand command) override {
        command.index=tags.at(tag).mapping.index; writes.push_back(command);
        return {true,"offline test accepted"};
    }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&, edge_gateway::PendingWriteCommand) override {
        throw std::runtime_error("UI must not use grouped writes for pause then stop");
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string&,const std::string&) const override {
        return edge_gateway::NullOpt;
    }
};

edge_gateway::ScadaScreen screen() {
    edge_gateway::ScadaScreen s; s.screenId="LocalControl";s.width=1920;s.height=1080;
    edge_gateway::ScadaWidget input;
    input.widgetId="manual-input"; input.type="qtInput";input.visible=true;
    input.geometry.x=20;input.geometry.y=100;input.geometry.width=600;input.geometry.height=100;
    input.action.type="writeSetpoint";input.action.tagId=powerTag;input.action.nodeId="edge-1";
    input.action.requiresConfirmation=true;
    input.properties["controlPauseGuard"]="true";
    input.properties["pauseGuardModeTag"]=modeTag;
    input.properties["pauseGuardAppliedTag"]=appliedTag;
    s.widgets.push_back(input);
    auto stop=input;stop.widgetId="pcs-stop";stop.type="qtButton";stop.geometry.y=300;
    stop.action.type="pulse";stop.action.tagId=stopTag;stop.action.value="1";
    s.widgets.push_back(stop);
    return s;
}

struct DialogPlan {
    int inputs=0, confirmations=0, warnings=0;
    bool changeDuringInput=false, changeDuringConfirmation=false, expireDuringConfirmation=false;
    bool cancel=false;
};

void runDialogs(FakeRuntime& runtime, DialogPlan& plan, const std::function<void()>& operation) {
    QTimer timer;
    QObject::connect(&timer,&QTimer::timeout,[&]() {
        auto* active=QApplication::activeModalWidget();
        if(auto* input=qobject_cast<QInputDialog*>(active)) {
            ++plan.inputs;
            if(plan.changeDuringInput) runtime.values[modeTag].value=2;
            input->setDoubleValue(12.3);input->accept();
        } else if(auto* message=qobject_cast<QMessageBox*>(active)) {
            if(message->standardButtons() & QMessageBox::Yes) {
                ++plan.confirmations;
                if(plan.changeDuringConfirmation) runtime.values[modeTag].value=3;
                if(plan.expireDuringConfirmation) runtime.values[appliedTag].stale=true;
                message->button(plan.cancel ? QMessageBox::No : QMessageBox::Yes)->click();
            } else {
                if(message->icon()==QMessageBox::Warning) ++plan.warnings;
                message->accept();
            }
        }
    });
    timer.start(1);
    operation();
    timer.stop();
}
} // namespace

class ScadaSceneViewTestAccess {
public:
    static bool allowed(ScadaSceneView& view, const std::string& a=modeTag, const std::string& b=appliedTag) {
        return view.pauseGuardSatisfied(a,b,QDateTime::currentMSecsSinceEpoch());
    }
    static void edit(ScadaSceneView& view) {
        for(auto& widget:view.runtimeWidgets_) if(widget.inputTagId==powerTag) {view.editInput(widget);return;}
        throw std::runtime_error("input not wired into scene");
    }
};

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        FakeRuntime runtime;
        ScadaSceneView view(screen(),"",{},{},runtime,[](const std::string&){});
        require(ScadaSceneViewTestAccess::allowed(view),"fresh paused control rejected");
        require(!ScadaSceneViewTestAccess::allowed(view,"",appliedTag),"missing guard tag accepted");
        require(!ScadaSceneViewTestAccess::allowed(view,modeTag,modeTag),"aliased guard accepted");
        for(const auto& tag:{modeTag,appliedTag}) {
            for(double mode:{1.,2.,3.,-1.,0.001,std::numeric_limits<double>::quiet_NaN()}) {
                runtime.values[tag].value=mode;
                require(!ScadaSceneViewTestAccess::allowed(view),"nonzero/invalid mode accepted");
            }
            runtime.values[tag].value=0;
            runtime.values[tag].quality=0;
            require(!ScadaSceneViewTestAccess::allowed(view),"bad quality accepted");
            runtime.values[tag].quality=1;runtime.values[tag].stale=true;
            require(!ScadaSceneViewTestAccess::allowed(view),"stale value accepted");
            runtime.values[tag].stale=false;runtime.values[tag].expireAt=1;
            require(!ScadaSceneViewTestAccess::allowed(view),"expired value accepted");
            runtime.values[tag].expireAt=0;
            const auto value=runtime.values[tag];runtime.values.erase(tag);
            require(!ScadaSceneViewTestAccess::allowed(view),"missing value accepted");runtime.values[tag]=value;
            runtime.tags[tag].tag.nodeId="other";
            require(!ScadaSceneViewTestAccess::allowed(view),"remote guard accepted");runtime.tags[tag].tag.nodeId=runtime.node;
        }
        for(int scenario=0;scenario<8;++scenario) {
            FakeRuntime r;
            if(scenario==1) r.values[modeTag].value=2;
            if(scenario==2) r.values[appliedTag].value=3;
            if(scenario==6) r.tags[powerTag].writeMaxValue=edge_gateway::NullOpt;
            ScadaSceneView v(screen(),"",{},{},r,[](const std::string&){});
            DialogPlan plan;
            plan.changeDuringInput=scenario==3;plan.changeDuringConfirmation=scenario==4;
            plan.expireDuringConfirmation=scenario==5;plan.cancel=scenario==7;
            runDialogs(r,plan,[&]{ScadaSceneViewTestAccess::edit(v);});
            std::cout<<"edit scenario="<<scenario<<" writes="<<r.writes.size()
                     <<" inputs="<<plan.inputs<<" confirmations="<<plan.confirmations
                     <<" warnings="<<plan.warnings<<std::endl;
            require(r.writes.size()==(scenario==0?1u:0u),"manual edit wrote through guard/cancel");
            if(scenario==0) require(r.writes[0].index==740101 && std::fabs(r.writes[0].value-12.3)<1e-9,
                "wrong manual target or value");
            if(scenario==1 || scenario==2 || scenario==6) require(plan.inputs==0 && plan.warnings==1,"editor opened before guard");
        }
        for(int scenario=0;scenario<5;++scenario) {
            FakeRuntime r;
            if(scenario==1) r.values[modeTag].value=3;
            if(scenario==2) r.values[appliedTag].value=3;
            ScadaSceneView v(screen(),"",{},{},r,[](const std::string&){});
            DialogPlan plan;plan.changeDuringConfirmation=scenario==3;plan.expireDuringConfirmation=scenario==4;
            QPushButton* button=nullptr;
            for(auto* item:v.scene()->items()) {
                auto* proxy=qgraphicsitem_cast<QGraphicsProxyWidget*>(item);
                if(proxy && proxy->widget()->objectName()=="pcs-stop") button=qobject_cast<QPushButton*>(proxy->widget());
            }
            require(button!=nullptr,"stop button missing");
            runDialogs(r,plan,[&]{button->click();});
            require(r.writes.size()==(scenario==0?1u:0u),"stop wrote before pause applied or after mode change");
            if(scenario==0) require(r.writes[0].index==1202 && r.writes[0].value==1,"stop must only submit its own command");
        }
        std::cout<<"scada_pause_guard_test passed: guard matrix, 8 edit and 5 stop scenarios\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"scada_pause_guard_test failed: "<<e.what()<<'\n';return 1;
    }
}
